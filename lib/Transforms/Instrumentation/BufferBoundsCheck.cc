/*
   Instrument a program to add buffer overflow/ underflow checks.

   For each pointer dereference *p we add two shadow registers:
   p.offset and p.size. p.offset is the offset from the base address
   of the object that contains p and p.size is the actual size of the
   allocated memory for p (including padding). Note that for stack and
   static allocations p.size is always know but for malloc-like
   allocations p.size may be unknown.

   Then, for each pointer dereference *p we add two assertions:
     [underflow]  assert (p.offset >= 0)
     [overflow ]  assert (p.offset < p.size)

   For instrumenting a function f we add for each dereferenceable
   formal parameter x two more shadow formal parameters x.offset and
   x.size. Then, at a call site of f and for a dereferenceable actual
   parameter y we add its corresponding y.offset and y.size. Moreover,
   for each function that returns a pointer we add two more shadow
   formal parameters to represent the size and offset of the returned
   value. The difference here is that these two shadow variables must
   be passed by reference at the call site so the continuation can use
   those. Thus, rather than using registers we allocate them in the
   stack and pass their addresses to the callee.

   If the instrumented program does not violate any of the assertions
   then the original program is free of buffer overflows/underflows.

   TODO:
     - instrument loads that return memory addresses .
*/

#include "seahorn/Transforms/Instrumentation/BufferBoundsCheck.hh"
#include "seahorn/Transforms/Instrumentation/ShadowBufferBoundsCheckFuncPars.hh"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"

#include <boost/optional.hpp>

#include "avy/AvyDebug.h"
//#include "seahorn/Analysis/Steensgaard.hh"

static llvm::cl::opt<bool>
InlineChecks("boc-inline-all",
             llvm::cl::desc ("Insert checks with assuming all functions have been inlined."),
             llvm::cl::init (false));

namespace seahorn
{
using namespace llvm;

char BufferBoundsCheck::ID = 0;

inline bool isUnknownSize (uint64_t sz)
{
	return sz == AliasAnalysis::UnknownSize;
}

inline bool isScalarGlobal(const Value* V)
{
	if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(V))
	{
		return (GV->getType ()->getContainedType (0)->isFloatingPointTy() ||
		        GV->getType ()->getContainedType (0)->isIntegerTy ());
	}
	else return false;
}

// uint64_t BufferBoundsCheck::getDSNodeSize (const Value *V, DSGraph *dsg, DSGraph *gDsg)
// {
//   // DSNode* n = dsg->getNodeForValue (V).getNode ();
//   // if (!n) n = gDsg->getNodeForValue (V).getNode ();
//   // if (!n) return AliasAnalysis::UnknownSize;

//   //m_dsa->print (errs (), NULL);
//   //errs () << "Size: " << n->getSize () << "\n";
//   //return n->getSize ();


//   // TODO: n->getSize() doesn't return the expected size for arrays.
//   return AliasAnalysis::UnknownSize;
// }

Value* BufferBoundsCheck::createAdd(IRBuilder<>B,
                                    Value *LHS, Value *RHS,
                                    const Twine &Name)
{
	assert (LHS->getType ()->isIntegerTy () &&
	        RHS->getType ()->isIntegerTy ());

	Value *LHS1 = B.CreateZExtOrBitCast (LHS, m_Int64Ty);
	Value *RHS1 = B.CreateZExtOrBitCast (RHS, m_Int64Ty);

	return  B.CreateAdd ( LHS1, RHS1, Name);
}

Value* BufferBoundsCheck::createMul(IRBuilder<>B,
                                    Value *LHS, unsigned RHS,
                                    const Twine &Name)
{
	assert (LHS->getType ()->isIntegerTy ());

	Value* LHS1 = B.CreateZExtOrBitCast (LHS, m_Int64Ty );

	return  B.CreateMul ( LHS1,
	                      ConstantInt::get (m_Int64Ty, RHS),
	                      Name);
}


void BufferBoundsCheck::resolvePHIUsers (const Value *v,
        DenseMap <const Value*, Value*>& m_table)
{
	// resolve phi incoming values that were marked as undef
	for (Value::const_use_iterator it = v->use_begin(), et = v->use_end (); it != et; ++it)
	{
		if (const PHINode *PHI = dyn_cast<PHINode> (*it))
		{
			Value * ValShadow = m_table [*it];
			if (!ValShadow) continue;

			if (PHINode *PHIShadow = dyn_cast<PHINode> (ValShadow))
			{
				for (unsigned i = 0; i < PHI->getNumIncomingValues (); i++)
				{
					if (PHI->getIncomingValue (i) == v &&
					        ( ( i >= PHIShadow->getNumIncomingValues ()) ||
					          PHIShadow->getIncomingValue (i) == UndefValue::get (m_Int64Ty)))
					{
						LOG ("boc", errs () << "Resolving " << *PHIShadow << "\n");
						PHIShadow->setIncomingValue (i, m_table [v]);
						LOG ("boc", errs () << "Replacing undef with " << * (m_table [v]) << "\n");
					}
				}
			}
		}
	}

}

void BufferBoundsCheck::instrumentGepOffset(IRBuilder<> B,
        Instruction* insertPoint,
        const GetElementPtrInst *gep)
{
	LOG ("boc" , errs () << "instrumenting GEP (offset) : " << *gep << "\n");

	SmallVector<const Value*, 4> ps;
	SmallVector<const Type*, 4> ts;
	gep_type_iterator typeIt = gep_type_begin (*gep);
	for (unsigned i = 1; i < gep->getNumOperands (); ++i, ++typeIt)
	{
		ps.push_back (gep->getOperand (i));
		ts.push_back (*typeIt);
	}

	const Value *base = gep->getPointerOperand ();

	Value *gepBaseOff = m_offsets [base];

	if (!gepBaseOff)
	{
		LOG ("boc", errs () << "Cannot determine the base offset for ";
		     errs () << *base << "\n");
		return;
	}

	B.SetInsertPoint(insertPoint);

	Value* curVal = gepBaseOff;

	LOG ("boc", errs() << "Offset=" << *curVal << " ");

	for (unsigned i = 0; i < ps.size (); ++i)
	{
		if (const StructType *st = dyn_cast<const StructType> (ts [i]))
		{
			if (const ConstantInt *ci = dyn_cast<const ConstantInt> (ps [i]))
			{
				unsigned off = fieldOffset (st, ci->getZExtValue ());
				curVal = createAdd( B,
				                    curVal,
				                    ConstantInt::get (m_Int64Ty, off));

				LOG ("boc", errs ()  << " + " << off );
			}
			else assert (false);
		}
		else if (const SequentialType *seqt = dyn_cast<const SequentialType> (ts [i]))
		{
			// arrays, pointers, and vectors

			unsigned sz = storageSize (seqt->getElementType ());

			LOG ("boc", errs () << " + " << " (" << *ps[i]  << " * " << sz << ") ");

			Value *LHS = curVal;

			Value *RHS = createMul(B,
			                       const_cast<Value*> (ps[i]),
			                       sz);

			curVal = createAdd(B, LHS, RHS);

		}
	}
	LOG ("boc", errs () << "\n");

	m_offsets [gep] = curVal;

	resolvePHIUsers (gep, m_offsets);
}


/*

  This instruments the offset and size of ptr by inserting
  arithmetic instructions. We instrument ptr as long as it follows a
  sequence of instructions with this grammar:

  Ptr = globalVariable | alloca | malloc | load | inttoptr | formal param | return |
        (getElementPtr (Ptr) | bitcast (Ptr) | phiNode (Ptr) ... (Ptr) )*

 */
void BufferBoundsCheck::instrumentSizeAndOffsetPtr (Function *F,
        IRBuilder<> B,
        Instruction* insertPoint,
        const Value * ptr,
        ValueSet &visited
        /*DSGraph *dsg, DSGraph *gDsg*/)
{
	//printf("Inside instrumentSizeAndOffsetPtr");
	//const Value *tmp = visited.find(ptr);
	//if (visited.find(ptr) != visited.end())  return;
	visited.insert (ptr);

	/// recursive cases

	if (const BitCastInst *Bc = dyn_cast<BitCastInst> (ptr))
	{

		Instruction *insertPoint = const_cast<Instruction*> (cast<Instruction> (Bc));

		instrumentSizeAndOffsetPtr (F, B, insertPoint,
		                            Bc->getOperand (0),
		                            visited
		                            /*,dsg, gDsg*/);

		B.SetInsertPoint(insertPoint);

		if (Value* shadowBcOpOff = lookupOffset(Bc->getOperand (0)))
			m_offsets [ptr] = shadowBcOpOff;

		if (Value* shadowBcOpSize = lookupSize(Bc->getOperand (0)))
			m_sizes [ptr] = shadowBcOpSize;

		return;
	}

	if (const GetElementPtrInst *Gep = dyn_cast<GetElementPtrInst> (ptr))
	{

		Instruction *insertPoint = const_cast<Instruction*> (cast<Instruction> (Gep));

		instrumentSizeAndOffsetPtr (F, B, insertPoint,
		                            Gep->getPointerOperand (),
		                            visited
		                            /*,dsg, gDsg*/);

		B.SetInsertPoint(insertPoint);

		instrumentGepOffset(B, insertPoint, Gep);

		// For debugging - delete these for release
		uint64_t offset_int = -1;
		// if (const ConstantInt *offset_val = dyn_cast<ConstantInt>(m_offsets[ptr])) {
		// 	offset_int = offset_val->getSExtValue();
		// }

		uint64_t Size = AliasAnalysis::UnknownSize;

		if (Value* shadowGepOpSize = lookupSize(Gep->getPointerOperand ()))
		{
			// shadowGepOpSize is in number of elements, NOT in bytes.
			// m_offsets[ptr] is in bytes. So multiply shadowGepOpSize by
			// bytes/element to get m_sizes[ptr] in bytes.
			//ConstantInt *op_size_corrected = ConstantInt::get()

			if (const ConstantInt *size_val = dyn_cast<ConstantInt>(shadowGepOpSize)) {
				uint64_t base_int = size_val->getSExtValue();
				int size_bytes = size_val->getBitWidth() / 8;
				// IntegerType *gep_size_type = size_val->getType();
				Constant * gep_size_corrected = ConstantInt::get(m_Int64Ty, base_int * size_bytes);
				m_sizes [ptr] = gep_size_corrected;
			}	else {
				m_sizes[ptr] = shadowGepOpSize;
			}
			// For debugging
			//ConstantInt *gep_size_ci = dyn_cast<ConstantInt>(gep_size_corrected);
			//uint64_t gep_size_int = gep_size_ci->getValue().getLimitedValue();

			resolvePHIUsers (ptr, m_sizes);

			LOG ("boc", errs() << "Size=" << * (m_sizes [ptr]) << "\n");
		}

		return;
	}

	if (const PHINode *PHI = dyn_cast<PHINode> (ptr))
	{

		Instruction *insertPoint = const_cast<Instruction*> (cast<Instruction> (PHI));

		PHINode* shadowPHINodeOff = PHINode::Create (m_Int64Ty, PHI->getNumIncomingValues (),
		                            ((ptr->hasName ()) ?
		                             ptr->getName () + ".shadow.offset" : ""),
		                            insertPoint);

		PHINode* shadowPHINodeSize = PHINode::Create (m_Int64Ty, PHI->getNumIncomingValues (),
		                             ((ptr->hasName ()) ?
		                              ptr->getName () + ".shadow.size" : ""),
		                             insertPoint);

		const Value *known_good = 0;
		for (unsigned i = 0; i < PHI->getNumIncomingValues (); i++)
		{
			Instruction *curr_phi_val = dyn_cast<Instruction> (PHI->getIncomingValue (i));
			if (!curr_phi_val)
				curr_phi_val = PHI->getIncomingBlock (i)->getFirstNonPHI ();
			ValueSet visited_2;

			instrumentSizeAndOffsetPtr(F, B, insertPoint, curr_phi_val, visited_2);
			// placeholder for now
			// shadowPHINodeOff->addIncoming (UndefValue::get (m_Int64Ty), PHI->getIncomingBlock (i));
			// shadowPHINodeSize->addIncoming (UndefValue::get (m_Int64Ty), PHI->getIncomingBlock (i));
			const Value* cur_phi_val_val = dyn_cast<Value>(curr_phi_val);

			// // Got a null response, so something couldn't be parsed.
			// if (!m_offsets[cur_phi_val_val] || !m_sizes[cur_phi_val_val]){
			// 	m_offsets[cur_phi_val_val] = m_offsets[known_good];
			// 	m_sizes[cur_phi_val_val] = m_sizes[known_good];
			// } else {
			// 	known_good = cur_phi_val_val;
			// }

			if (!m_offsets[cur_phi_val_val]) {
				m_offsets[cur_phi_val_val] = UndefValue::get (m_Int64Ty);
				//m_offsets[cur_phi_val_val] = ConstantInt::get(m_Int64Ty, AliasAnalysis::UnknownSize);
			}
			if (!m_sizes[cur_phi_val_val]) {
				m_sizes[cur_phi_val_val] = UndefValue::get (m_Int64Ty);
				//m_sizes[cur_phi_val_val] = ConstantInt::get(m_Int64Ty,  AliasAnalysis::UnknownSize);
			}
			shadowPHINodeOff->addIncoming (m_offsets[cur_phi_val_val], PHI->getIncomingBlock(i));
			shadowPHINodeSize->addIncoming (m_sizes[cur_phi_val_val], PHI->getIncomingBlock(i));

		}


		m_offsets [ptr] = shadowPHINodeOff;
		m_sizes [ptr] = shadowPHINodeSize;

		for (unsigned i = 0; i < PHI->getNumIncomingValues (); i++)
		{

			Instruction *insertPoint = dyn_cast<Instruction> (PHI->getIncomingValue (i));
			if (!insertPoint)
				insertPoint = PHI->getIncomingBlock (i)->getFirstNonPHI ();

			instrumentSizeAndOffsetPtr (F, B, insertPoint,
			                            PHI->getIncomingValue (i),
			                            visited
			                            /*,dsg, gDsg*/);


			if (Value* shadowPHIValOff = lookupOffset(PHI->getIncomingValue (i)))
			{
				//shadowPHINodeOff->addIncoming (shadowPHIValOff, PHI->getIncomingBlock (i));
				shadowPHINodeOff->setIncomingValue (i, shadowPHIValOff);
				LOG ("boc", errs() << "Offset=" << *shadowPHIValOff << "\n");
			}
			// else
			// {
			//   // placeholder to be resolved later to break cycle
			//   Value *Undef = UndefValue::get (m_Int64Ty);
			//   shadowPHINodeOff->addIncoming (Undef, PHI->getIncomingBlock (i));
			// }

			if (Value* shadowPHIValSize = lookupSize(PHI->getIncomingValue (i)))
			{
				//shadowPHINodeSize->addIncoming (shadowPHIValSize, PHI->getIncomingBlock (i));
				shadowPHINodeSize->setIncomingValue (i, shadowPHIValSize);
				LOG ("boc", errs() << "Offset=" << *shadowPHIValSize << "\n");
			}
			// else
			// {
			//   // placeholder to be resolved later to break cycle
			//   Value *Undef = UndefValue::get (m_Int64Ty);
			//   shadowPHINodeSize->addIncoming (Undef, PHI->getIncomingBlock (i));
			// }
		}

		return;
	}

	if (const AllocaInst *alloca_inst = dyn_cast<AllocaInst> (ptr)) {

		uint64_t Size = AliasAnalysis::UnknownSize;
		getObjectSize(ptr, Size, m_dl, m_tli, false);
		if (!isUnknownSize(Size))
		{
			m_sizes[ptr] = ConstantInt::get (m_Int64Ty, Size);
			m_offsets[ptr] = ConstantInt::get (m_Int64Ty, 0);
			return;
		} else {
			const Value* next_pointer = alloca_inst->getArraySize();
			instrumentSizeAndOffsetPtr (F, B, insertPoint,
			                            next_pointer,
			                            visited);
			m_sizes[ptr] = m_sizes[next_pointer];
			m_offsets [ptr] = m_offsets[next_pointer];
			return;
		}
	}

	if (const LoadInst *load_inst = dyn_cast<LoadInst> (ptr)) {
		uint64_t Size = AliasAnalysis::UnknownSize;
		if ((ptr->getType()->isPtrOrPtrVectorTy())) {
			getObjectSize(ptr, Size, m_dl, m_tli, false);
		}

		if (!isUnknownSize(Size))
		{
			m_sizes [ptr] = ConstantInt::get (m_Int64Ty, Size);
			m_offsets [ptr] = ConstantInt::get (m_Int64Ty, 0);
			return;
		} else {
			const Value* next_pointer = load_inst->getPointerOperand();
			instrumentSizeAndOffsetPtr (F, B, insertPoint,
			                            next_pointer,
			                            visited);
			m_sizes[ptr] = m_sizes[next_pointer];
			m_offsets[ptr] = m_offsets[next_pointer];
			return;
		}
	}

	if (const StoreInst *store_inst = dyn_cast<StoreInst>(ptr)) {
		uint64_t Size = AliasAnalysis::UnknownSize;
		if ((ptr->getType()->isPtrOrPtrVectorTy())) {
			getObjectSize(ptr, Size, m_dl, m_tli, false);
		}

		if (!isUnknownSize(Size))
		{
			m_sizes [ptr] = ConstantInt::get (m_Int64Ty, Size);
			m_offsets [ptr] = ConstantInt::get (m_Int64Ty, 0);
			return;
		} else {
			const Value* next_pointer = store_inst->getValueOperand();
			instrumentSizeAndOffsetPtr (F, B, insertPoint,
			                            next_pointer,
			                            visited);
			m_sizes[ptr] = m_sizes[next_pointer];
			m_offsets[ptr] = m_offsets[next_pointer];
			return;
		}
	}

	// binary operator, like i++
	if (const BinaryOperator *bin_inst = dyn_cast<BinaryOperator> (ptr)) {
		llvm::Instruction::BinaryOps opcode = bin_inst->getOpcode();
		Value *first_op = bin_inst->getOperand(0);
		instrumentSizeAndOffsetPtr (F, B, insertPoint,
		                            first_op,
		                            visited);
		Value *second_op = bin_inst->getOperand(1);
		instrumentSizeAndOffsetPtr (F, B, insertPoint,
		                            second_op,
		                            visited);

		// Either one of the ops is null, can't compute the sum
		if (!m_sizes[first_op] || !m_sizes[second_op])
			return;

		ConstantInt *first_op_ci = dyn_cast<ConstantInt>(m_sizes[first_op]);
		ConstantInt *second_op_ci = dyn_cast<ConstantInt>(m_sizes[second_op]);
		uint64_t first_op_int = first_op_ci->getSExtValue();
		uint64_t second_op_int = second_op_ci->getSExtValue();

		if (opcode == llvm::Instruction::Add) {
			m_sizes[ptr] = ConstantInt::get(m_Int64Ty, first_op_int + second_op_int);
			m_offsets[ptr] = ConstantInt::get(m_Int64Ty, 0);
		} else if (opcode == llvm::Instruction::Sub) {
			m_sizes[ptr] = ConstantInt::get(m_Int64Ty, first_op_int + second_op_int);
			m_offsets[ptr] = ConstantInt::get(m_Int64Ty, 0);
		}
	}

	if (const SelectInst *sel_inst = dyn_cast<SelectInst>(ptr)) {

		// // create a PHI node
		// PHINode* shadow_phi = PHINode::Create (m_Int64Ty,2,
		//                             ((ptr->hasName ()) ?
		//                              ptr->getName () + "select instruction" : ""),
		//                             insertPoint);
		// BasicBlock *parent = (BasicBlock *)dyn_cast<BasicBlock> (sel_inst->getParent());
		// shadow_phi->addIncoming((Value *)sel_inst->getTrueValue(), parent);
		// shadow_phi->addIncoming((Value *)sel_inst->getFalseValue(), parent);
		// m_offsets[sel_inst] = ContantInt::get(m_Int64Ty, 0);
	}

	/// base cases
	if (const ConstantInt *constant = dyn_cast<ConstantInt>(ptr)) {
		m_sizes[ptr] = ConstantInt::get(m_Int64Ty, constant->getSExtValue());
		m_offsets[ptr] = ConstantInt::get(m_Int64Ty, 0);
	}

	if (isa<GlobalVariable> (ptr) ||
	        isAllocationFn (ptr, m_tli, true))
	{

		uint64_t Size = AliasAnalysis::UnknownSize;
		//getObjectSize(ptr, Size, m_dl, m_tli, false);
		if (const GlobalVariable *inst_global = dyn_cast<GlobalVariable>(ptr)) {
			if (inst_global->hasInitializer()) {
				bool is_const = inst_global->isConstant();
				const Constant *inst_val = inst_global->getInitializer();
				const ConstantInt *inst_val_int = dyn_cast<ConstantInt>(inst_val);
				//getObjectSize(ptr, Size, m_dl, m_tli, false);

				// m_sizes[ptr] = inst_val_int;
				uint64_t var_val = inst_val_int->getSExtValue();
				// Type *var_type = inst_val_int->getType();
				m_sizes[ptr] = ConstantInt::get(m_Int64Ty, var_val);
				m_offsets[ptr] = ConstantInt::get (m_Int64Ty, 0);
				return;
			}
		}

		m_offsets [ptr] = ConstantInt::get (m_Int64Ty, 0);
		if (!isUnknownSize(Size))
		{
			m_sizes [ptr] = ConstantInt::get (m_Int64Ty, Size);
			return;
		}
		else if (const AllocaInst *inst = dyn_cast<AllocaInst> (ptr)) {
			// bool is_array_allof = inst->isArrayAllocation();
			// const Value* alloc_inst = inst->getArraySize();

			// If the alloca does not have a static allocation size, then...go find it!
			if (!inst->isStaticAlloca()) {
				Instruction *insert_point = const_cast<Instruction*> (cast<Instruction> (inst));
				ValueSet visited;
				//instrumentAllocaSize(F, B, insertPoint, ptr, visited);
			}
			// return;
		}

		if (CallInst * MallocInst = extractMallocCall (const_cast<Value*> (ptr),
		                            m_tli))
		{
			if (MallocInst->getNumArgOperands () == 1)
			{
				Value *mallocArg = MallocInst->getArgOperand(0);

				// Size = getDSNodeSize(mallocArg, dsg, gDsg);
				// if (!isUnknownSize(Size))
				// {
				//   m_sizes [ptr] = ConstantInt::get (m_Int64Ty, Size);
				//   return;
				// }

				if (mallocArg->getType ()->isIntegerTy ())
				{
					m_sizes [ptr] = mallocArg;
					return;
				}
			}
		}
	}


	if (const IntToPtrInst *IP = dyn_cast<IntToPtrInst> (ptr))
	{
		m_offsets [ptr] = ConstantInt::get (m_Int64Ty, 0);
		unsigned Size = m_dl->getPointerTypeSizeInBits (IP->getType ());
		m_sizes [ptr] = ConstantInt::get (m_Int64Ty, Size);
		return;
	}

	if (!m_inline_all)
	{
		ShadowBufferBoundsCheckFuncPars &SBOA =
		    getAnalysis<ShadowBufferBoundsCheckFuncPars> ();

		B.SetInsertPoint(insertPoint);

		/// ptr is the return value of a call site
		if (const CallInst *CI = dyn_cast<CallInst> (ptr))
		{
			CallSite CS (const_cast<CallInst*> (CI));
			Function *cf = CS.getCalledFunction ();
			if (cf && SBOA.IsShadowableFunction (*cf))
			{
				Value* ShadowRetOff  = CS.getArgument (CS.arg_size () - 2);
				Value* ShadowRetSize = CS.getArgument (CS.arg_size () - 1);
				B.CreateCall (m_memsafeFn, ShadowRetOff);
				m_offsets [ptr] = B.CreateLoad (ShadowRetOff);
				B.CreateCall (m_memsafeFn, ShadowRetSize);
				m_sizes [ptr] = B.CreateLoad (ShadowRetSize);
				return;
			}
		}

		/// try if ptr is  a function formal parameter
		auto p =  SBOA.findShadowArg (F, ptr);
		Value* shadowPtrOff =  p.first;
		Value* shadowPtrSize = p.second;
		if (shadowPtrOff && shadowPtrSize)
		{
			m_offsets [ptr] = shadowPtrOff;
			m_sizes [ptr] = shadowPtrSize;
			return;
		}
	}

	LOG( "boc",
	     errs () << "Unable to instrument " << *ptr << "\n");
}

void BufferBoundsCheck::instrumentSizeAndOffsetPtr (Function *F, IRBuilder<> B,
        Instruction* insertPoint,
        const Value * ptr
        /*,DSGraph *dsg, DSGraph *gDsg*/)
{
	ValueSet visited;
	instrumentSizeAndOffsetPtr (F, B, insertPoint, ptr, visited/*, dsg, gDsg*/);
}

/*
For some AllocaInst alloc_inst, find the instruction that contains the size
of the allocation. ptr is the current instruction, F is the current function
*/
void BufferBoundsCheck::instrumentAllocaSize (Function *F,
        IRBuilder<> B,
        Instruction* insertPoint,
        const Value * ptr,
        ValueSet &visited)
{
	uint64_t Size = AliasAnalysis::UnknownSize;
	const Value *ptr_orig = ptr;
	getObjectSize(ptr, Size, m_dl, m_tli, false);

	while (isUnknownSize(Size)) {
		//Value *tmp = visited.find(ptr);

		if (const GetElementPtrInst *inst = dyn_cast<GetElementPtrInst> (ptr)) {
			ptr = inst->getPointerOperand();
		} else if (const LoadInst *inst = dyn_cast<LoadInst> (ptr)) {
			ptr = inst->getPointerOperand();
		} else if (const AllocaInst *inst = dyn_cast<AllocaInst> (ptr)) {
			ptr = inst->getArraySize();
		}

		// If ptr isn't a pointer, then we're (almost) done
		if ((ptr->getType()->isPtrOrPtrVectorTy()))  {
			Size = AliasAnalysis::UnknownSize;
			getObjectSize(ptr, Size, m_dl, m_tli, false);
		} else if (const GlobalVariable *ptr_operand = dyn_cast<GlobalVariable>(ptr)) {
			// This is here for debugging
			const Constant* ptr_val = ptr_operand->getInitializer();
		}
		visited.insert(ptr);
	}

	m_sizes[ptr_orig] = ConstantInt::get (m_Int64Ty, Size);
	//m_offsets[ptr_orig] = ConstantInt::get (m_Int64Ty, 0);

}

//! instrument check for load, store and memset
bool BufferBoundsCheck::instrumentCheck (IRBuilder<> B,
        Instruction& inst,
        const Value& ptr)
{
	Value *ptrSize   = m_sizes [&ptr];
	Value *ptrOffset = m_offsets [&ptr];

	if (!(ptrSize && ptrOffset))
	{
		ChecksUnable++;
		return false;
	}

	if (ConstantInt *size_ci = dyn_cast<ConstantInt>(ptrSize)) {
		if (size_ci->getSExtValue() == AliasAnalysis::UnknownSize)
			return false;
	}

	if (ConstantInt *offset_ci = dyn_cast<ConstantInt>(ptrOffset)) {
		if (offset_ci->getSExtValue() == AliasAnalysis::UnknownSize)
			return false;
	}

	B.SetInsertPoint (&inst);

	// It's tempting to generate Cmp1 and Cmp2 and conjoin them in an
	// And instruction. However, this is not code that we want to give
	// to a standard abstract interpreter.

	/// Underflow: add check ptrOffset >= 0

	BasicBlock *OldBB0 = inst.getParent ();
	BasicBlock *Cont0 = OldBB0->splitBasicBlock(B.GetInsertPoint ());
	OldBB0->getTerminator ()->eraseFromParent ();
	BranchInst::Create (Cont0, OldBB0);

	B.SetInsertPoint (Cont0->getFirstNonPHI ());

	Value* Cmp1 = B.CreateICmpSGE (ptrOffset,
	                               ConstantInt::get (m_Int64Ty, 0),
	                               "BOA_underflow");

	BasicBlock *OldBB1 = Cont0;
	BasicBlock *Cont1 = OldBB1->splitBasicBlock(B.GetInsertPoint ());
	OldBB1->getTerminator ()->eraseFromParent();
	BranchInst::Create (Cont1, m_err_bb, Cmp1, OldBB1);

	/// Overflow: add check ptrOffset < ptrSize

	B.SetInsertPoint (Cont1->getFirstNonPHI ());

	Value* Cmp2 = B.CreateICmpSLT (ptrOffset,
	                               ptrSize,
	                               "BOA_overflow");

	BasicBlock *OldBB2 = Cont1;
	BasicBlock *Cont2 = OldBB2->splitBasicBlock(B.GetInsertPoint ());
	OldBB2->getTerminator ()->eraseFromParent();
	BranchInst::Create (Cont2, m_err_bb, Cmp2, OldBB2);

	ChecksAdded++;

	LOG ("boc" , errs () << "\nInserted:\n";
	     errs () << "\t" << "assert(" << *ptrOffset << " >= 0)\n";
	     errs () << "\t" << "assert(" << *ptrOffset << " < " << *ptrSize << ")\n");

	return true;
}


//! instrument check for memcpy and memmove
bool BufferBoundsCheck::instrumentCheck (IRBuilder<> B,
        Instruction& inst,
        const Value& ptr,
        const Value& len)
{
	Value *ptrSize   = m_sizes [&ptr];
	Value *ptrOffset = m_offsets [&ptr];

	if (!(ptrSize && ptrOffset))
	{
		ChecksUnable++;
		return false;
	}

	B.SetInsertPoint (&inst);

	BasicBlock *OldBB0 = inst.getParent ();
	BasicBlock *Cont0 = OldBB0->splitBasicBlock(B.GetInsertPoint ());
	OldBB0->getTerminator()->eraseFromParent ();
	BranchInst::Create(Cont0, OldBB0);

	B.SetInsertPoint (Cont0->getFirstNonPHI ());

	// check underflow ptrOffset >= 0
	Value* Cmp1 = B.CreateICmpSGE (ptrOffset,
	                               ConstantInt::get (m_Int64Ty, 0),
	                               "BOA_underflow");


	BasicBlock *OldBB1 = Cont0;
	BasicBlock *Cont1 = OldBB1->splitBasicBlock(B.GetInsertPoint ());
	OldBB1->getTerminator()->eraseFromParent();
	BranchInst::Create(Cont1, m_err_bb, Cmp1, OldBB1);

	/// Add check ptrOffset + len <= ptrSize

	B.SetInsertPoint (Cont1->getFirstNonPHI ());

	Value *rng = createAdd (B, ptrOffset, const_cast<Value*> (&len));
	Value* Cmp2 = B.CreateICmpSLE (rng, ptrSize, "BOA_overflow");

	BasicBlock *OldBB2 = Cont1;
	BasicBlock *Cont2 = OldBB2->splitBasicBlock(B.GetInsertPoint ());
	OldBB2->getTerminator ()->eraseFromParent();
	BranchInst::Create (Cont2, m_err_bb, Cmp2, OldBB2);

	ChecksAdded++;

	LOG ("boc" , errs () << "\nInserted:\n";
	     errs ()
	     << "\t" << "assert(" << *ptrOffset << " >= 0 \n"
	     << "\t" << "assert(" << *ptrOffset << " + " << len
	     << " <= " << *ptrSize << ")\n");

	return true;
}

void BufferBoundsCheck::instrumentErrAndSafeBlocks (IRBuilder<>B,
        Function &F)
{
	//printf("Inside instrumentErrAndSafeBlocks");
	LLVMContext &ctx = B.getContext ();

	m_err_bb = BasicBlock::Create(ctx, "Error", &F);
	B.SetInsertPoint (m_err_bb);
	B.CreateCall (m_errorFn);
	B.CreateUnreachable ();
	return;

	// // The original return statement is replaced with a block with an
	// // infinite loop while a fresh block named ERROR returning an
	// // arbitrary value is created. All unsafe checks jump to ERROR.
	// // The original program has been fully inlined and the only
	// // function is "main" which should return an integer.

	// BasicBlock * retBB = nullptr;
	// ReturnInst *retInst = nullptr;
	// for (BasicBlock& bb : F)
	// {
	//   TerminatorInst * inst = bb.getTerminator ();
	//   if (inst && (retInst = dyn_cast<ReturnInst> (inst)))
	//   {
	//     retBB = &bb;
	//     break;
	//   }
	// }

	// if (!retInst)
	// {
	//   if (F.getReturnType ()->isIntegerTy ())
	//   {
	//     m_err_bb = BasicBlock::Create(ctx, "Error", &F);
	//     B.SetInsertPoint (m_err_bb);
	//     B.CreateRet ( ConstantInt::get (F.getReturnType (), 42));

	//   }
	//   else
	//     assert (false &&
	//             "Only instrument functions that return an integer");
	// }
	// else
	// {
	//   Value * retVal = retInst->getReturnValue ();

	//   if (retVal && retVal->getType ()->isIntegerTy ())
	//   {
	//     m_err_bb = BasicBlock::Create(ctx, "ERROR", &F);
	//     B.SetInsertPoint (m_err_bb);
	//     B.CreateRet ( ConstantInt::get (retVal->getType (), 42));

	//   }
	//   else
	//   {
	//     assert ( false &&
	//             "Only instrument functions that return an integer");
	//   }

	//   // replace original return with an infinite loop

	//   B.SetInsertPoint (retInst);
	//   BasicBlock::iterator It = B.GetInsertPoint ();
	//   m_safe_bb = retBB->splitBasicBlock(It, "SAFE");
	//   BranchInst *loopInst = BranchInst::Create(m_safe_bb);
	//   ReplaceInstWithInst(retInst, loopInst);
	// }
}

bool BufferBoundsCheck::runOnFunction (Function &F)
{
	//printf("Inside runOnFunction");
	if (F.isDeclaration ()) return false;

	if (m_inline_all && !F.getName ().equals ("main"))
	{
		errs () << "Warning: " << F.getName () << " is not instrumented ";
		errs () << "only main is instrumented\n";
		return false;
	}

	// DSGraph* dsg = m_dsa->getDSGraph (F);
	// if (!dsg) return false;
	// DSGraph* gDsg = dsg->getGlobalsGraph ();

	LLVMContext &ctx = F.getContext ();
	IRBuilder<> B (ctx);

	instrumentErrAndSafeBlocks (B, F);
	assert (m_err_bb);

	// WorkList contains only load, store, call or return instructions
	// This only sees 3 call and 1 ret instruction, it misses all of the loads and stores!!!
	std::vector<Instruction*> WorkList;
	for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i)
	{
		Instruction *I = &*i;
		if (isa<LoadInst> (I) || isa<StoreInst>  (I) ||
		        isa<CallInst> (I) || isa<ReturnInst> (I))
			WorkList.push_back(I);
	}


	bool change = false;
	bool is_memsafe = false;
	for (auto inst : WorkList)
	{
		if (const CallInst *CI = dyn_cast<CallInst> (inst))
		{
			CallSite CS (const_cast<CallInst*> (CI));
			const Function *cf = CS.getCalledFunction ();
			if (cf)
			{
				if (cf->getName ().startswith ("verifier.memsafe"))
				{  is_memsafe = true; }
				else if (cf->getName ().startswith ("llvm.memcpy") ||
				         cf->getName ().startswith ("llvm.memmove"))
				{
					LOG ("boc" ,
					     errs () << "\n ================= \n";
					     errs () << "Found memcpy/memmove: " << *inst << "\n");

					Value* DestPtr = CS.getArgument (0);
					Value* SrcPtr  = CS.getArgument (1);
					Value* Len     = CS.getArgument (2);

					instrumentSizeAndOffsetPtr (&F, B, inst, SrcPtr);
					instrumentSizeAndOffsetPtr (&F, B, inst, DestPtr);

					change |=  instrumentCheck (B, *inst, *SrcPtr, *Len);
					change |=  instrumentCheck (B, *inst, *DestPtr, *Len);
				}
				else if (cf->getName ().startswith ("llvm.memset"))

				{
					LOG ("boc" ,
					     errs () << "\n ================= \n";
					     errs () << "Found memset: " << *inst << "\n");

					Value* DestPtr = CS.getArgument (0);
					Value* Len    = CS.getArgument (2);

					instrumentSizeAndOffsetPtr (&F, B, inst, DestPtr);
					change |=  instrumentCheck (B, *inst, *DestPtr, *Len);
				}
				else
				{
					if (!m_inline_all)
					{
						ShadowBufferBoundsCheckFuncPars &SBOA =
						    getAnalysis<ShadowBufferBoundsCheckFuncPars> ();

						// Resolving the shadow offsets and sizes which are
						// actual parameters of a function call
						if (SBOA.IsShadowableFunction (*cf))
						{
							size_t orig_arg_size = SBOA.getOrigArgSize (*cf);
							unsigned shadow_idx = orig_arg_size;
							for (size_t idx = 0; idx < orig_arg_size; idx++)
							{
								const Value* ArgPtr = CS.getArgument (idx);
								// this could be a symptom of a bug
								if (isa<UndefValue> (ArgPtr) || isa<ConstantPointerNull> (ArgPtr))
									continue;
								if (SBOA.IsShadowableType (ArgPtr->getType ()))
								{
									instrumentSizeAndOffsetPtr (&F, B, inst, ArgPtr);
									Value *ptrSize   = m_sizes [ArgPtr];
									Value *ptrOffset = m_offsets [ArgPtr];
									if (ptrSize && ptrOffset)
									{
										CS.setArgument (shadow_idx, ptrOffset);
										CS.setArgument (shadow_idx + 1, ptrSize);
										change = true;
									}
									shadow_idx += 2;
								}
							}
						}
					}
				}
			}
		}
		else if (const ReturnInst *ret = dyn_cast<ReturnInst> (inst))
		{
			if (!m_inline_all)
			{
				if (const Value* retVal = ret->getReturnValue ())
				{
					ShadowBufferBoundsCheckFuncPars &SBOA =
					    getAnalysis<ShadowBufferBoundsCheckFuncPars> ();
					if (SBOA.IsShadowableType (retVal->getType ()))
					{
						// Resolving the shadow offset and size of the return
						// value of a function
						instrumentSizeAndOffsetPtr (&F, B, inst, retVal);
						Value *ShadowOffset = m_offsets [retVal];
						Value *ShadowSize   = m_sizes [retVal];
						if (ShadowOffset && ShadowSize)
							change |= SBOA.resolveShadowRetDefs (&F, ShadowOffset, ShadowSize);
					}
				}
			}
		}
		else if (const LoadInst *load = dyn_cast<LoadInst> (inst))
		{
			if (is_memsafe)
			{	// a load inserted by intrumentation which is known as safe
				is_memsafe = false;
				continue;
			}

			LOG ("boc" ,
			     errs () << "\n ================= \n";
			     errs () << "Found load: " << *inst << "\n");

			const Value * Ptr = load->getOperand (0);
			if (isScalarGlobal (Ptr))
			{
				LOG ("boc", errs () << "Skipped load from scalar global " << *Ptr << "\n");
				ChecksSkipped++;
			}
			else
			{
				instrumentSizeAndOffsetPtr (&F, B, inst, Ptr/*, dsg, gDsg*/);
				change |=  instrumentCheck (B, *inst, *Ptr);
			}
		}
		else if (const StoreInst *store = dyn_cast<StoreInst> (inst))
		{
			if (is_memsafe)
			{	// a store inserted by intrumentation which is known as safe
				is_memsafe = false;
				continue;
			}

			LOG ("boc" ,
			     errs () << "\n ================= \n";
			     errs () << "Found store: " << *inst << "\n");


			const Value * Ptr = store->getOperand (1);
			if (isScalarGlobal (Ptr))
			{
				LOG ("boc", errs () << "Skipped store to scalar global " << *Ptr << "\n");
				ChecksSkipped++;
			}
			else
			{
				instrumentSizeAndOffsetPtr (&F, B, inst, Ptr/*, dsg, gDsg*/);
				change |=  instrumentCheck (B, *inst, *Ptr);
			}
		}
	}

	return change;
}

bool BufferBoundsCheck::runOnModule (llvm::Module &M)
{
	if (M.begin () == M.end ()) return false;

	m_dl = &getAnalysis<DataLayoutPass>().getDataLayout ();

	m_tli = &getAnalysis<TargetLibraryInfo>();

	//m_dsa = &getAnalysis<SteensgaardDataStructures> ();

	LLVMContext &ctx = M.getContext ();

	if (!m_inline_all) m_inline_all = InlineChecks;

	// ObjectSizeOffsetEvaluator TheObjSizeEval (m_dl, m_tli, ctx, true);
	// m_obj_size_eval = &TheObjSizeEval;

	m_Int64Ty = Type::getInt64Ty (ctx);
	m_Int64PtrTy = m_Int64Ty->getPointerTo ();

	AttrBuilder B;
	B.addAttribute (Attribute::NoReturn);
	// B.addAttribute (Attribute::ReadNone);

	AttributeSet as = AttributeSet::get (ctx,
	                                     AttributeSet::FunctionIndex,
	                                     B);

	m_errorFn = dyn_cast<Function>
	            (M.getOrInsertFunction ("verifier.error",
	                                    as,
	                                    Type::getVoidTy (ctx), NULL));

	B.clear ();
	B.addAttribute (Attribute::NoReturn);
	// B.addAttribute (Attribute::ReadNone);

	as = AttributeSet::get (ctx,
	                        AttributeSet::FunctionIndex,
	                        B);

	m_memsafeFn = dyn_cast<Function>
	              (M.getOrInsertFunction ("verifier.memsafe",
	                                      as,
	                                      Type::getVoidTy (ctx),
	                                      m_Int64PtrTy,
	                                      NULL));

	bool change = false;

	for (Function &F : M) change |= runOnFunction (F);

	LOG( "boc-stats",
	     errs ()
	     << "[BOA] checks added: " << ChecksAdded << "\n"
	     << "[BOA] checks unabled to add : " << ChecksUnable << " (should be =0)\n");

	return change;
}

void BufferBoundsCheck::getAnalysisUsage (llvm::AnalysisUsage &AU) const
{
	AU.setPreservesAll ();
	//AU.addRequiredTransitive<llvm::SteensgaardDataStructures> ();

	AU.addRequired<llvm::DataLayoutPass>();
	AU.addRequired<llvm::TargetLibraryInfo>();
	AU.addRequired<llvm::UnifyFunctionExitNodes> ();
	AU.addRequired<ShadowBufferBoundsCheckFuncPars>();
}


}

static llvm::RegisterPass<seahorn::BufferBoundsCheck>
X ("boc", "Insert buffer overflow/underflow checks");


