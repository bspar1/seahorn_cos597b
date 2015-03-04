#include "seahorn/HornifyFunction.hh"
#include "seahorn/LiveSymbols.hh"
#include "seahorn/Support/CFG.hh"
#include "seahorn/Support/ExprSeahorn.hh"

namespace seahorn
{

  /// Find a function exit basic block.  Assumes that the function has
  /// a unique block with return instruction
  static const BasicBlock* findExitBlock (const Function &F)
  {
    for (const BasicBlock& bb : F)
      if (isa<ReturnInst> (bb.getTerminator ())) return &bb;
    return NULL;
  }
  
  void HornifyFunction::extractFunctionInfo (const BasicBlock &BB)
  {
    const ReturnInst *ret = dyn_cast<const ReturnInst> (BB.getTerminator ());
    // not an exit block
    if (!ret) return;
    
    const Function &F = *BB.getParent ();
    // main does not need a summary
    if (F.getName ().equals ("main")) return;
    
    FunctionInfo &fi = m_sem.getFunctionInfo (F);
    
    // reserved arguments: 
    //  1. enabled flag
    //  2. incoming value of error.flag
    //  3. outgoing value of error.flag
    // the first two arguments are for the error flag
    Expr boolSort = sort::boolTy (m_efac);
    ExprVector sorts {boolSort, boolSort, boolSort};
    
    // memory regions
    for (const Instruction &inst : BB)
    {
      if (const CallInst *ci = dyn_cast<const CallInst> (&inst))
      {
        CallSite CS (const_cast<CallInst*> (ci));
        const Function *cf = CS.getCalledFunction ();
        if (cf && (cf->getName ().equals ("shadow.mem.in") || 
                   cf->getName ().equals ("shadow.mem.out")))
          {
            const Value &v = *CS.getArgument (1);
            Expr r = m_sem.symb (v);
            if (!r) continue;
            fi.regions.push_back (&v);
            sorts.push_back (bind::typeOf (r));
          }
      }
    }
    
    const ExprVector &live = m_parent.live (BB);
    
    // live arguments
    for (const Argument &arg : boost::make_iterator_range (F.arg_begin (), F.arg_end ()))
    {
      if (!m_sem.isTracked (arg)) continue;
      Expr v = m_sem.symb (arg);
      if (!v) continue;
      
      if (! std::binary_search (live.begin (), live.end (), v) ) continue;
      
      fi.args.push_back (&arg);
      sorts.push_back (bind::typeOf (v));
    }
   
    // live globals 
    for (Expr v : live)
    {
      Expr u = bind::fname (bind::fname (v));
      if (!isOpX<VALUE> (u)) continue;
      
      const Value *val = getTerm<const Value*> (u);
      if (!m_sem.isTracked (*val)) continue;
      if (const GlobalVariable *gv = dyn_cast<const GlobalVariable> (val))
      {
        fi.globals.push_back (gv);
        sorts.push_back (bind::typeOf (v));
      }
    }
    
    // return value
    fi.ret = ret->getReturnValue ();
    if (fi.ret)
    {
      // evaluate return value and get the sort from the value
      Expr v = m_sem.symb (*fi.ret);
      if (v) sorts.push_back (bind::typeOf (v));
      else fi.ret = NULL; // not tracked
    }
    
    sorts.push_back (mk<BOOL_TY> (m_efac));
    fi.sumPred = bind::fdecl (mkTerm<const Function*>(&F, m_efac), sorts);
    
    m_fp.registerRelation (fi.sumPred);
    
    // basic rules
    // if error.flag is on, it remains on, even if S is disabled
    //   S (true, true, true, V). 
    //   S (false, true, true, V).
    // if S is disabled, error.flag is unchanged
    //   S (false, false, false, V).
    SymStore s (m_efac);
    ExprSet allVars;
    Expr trueE = mk<TRUE> (m_efac);
    Expr falseE = mk<FALSE> (m_efac);
    ExprVector postArgs {trueE, trueE, trueE};
    fi.evalArgs (m_sem, s, std::back_inserter (postArgs));
    std::copy_if (postArgs.begin () + 3, postArgs.end (), 
                  std::inserter (allVars, allVars.begin ()),
                  bind::IsConst());
    
    m_fp.addRule (allVars, bind::fapp (fi.sumPred, postArgs));
    
    postArgs[0] = falseE;
    m_fp.addRule (allVars, bind::fapp (fi.sumPred, postArgs));
    
    postArgs[1] = falseE;
    postArgs[2] = falseE;
    m_fp.addRule (allVars, bind::fapp (fi.sumPred, postArgs));

    // -- expose basic properties of the summary
    postArgs [0] = bind::boolConst (mkTerm (std::string ("arg.0"), m_efac));
    postArgs [1] = bind::boolConst (mkTerm (std::string ("arg.1"), m_efac));
    postArgs [2] = bind::boolConst (mkTerm (std::string ("arg.2"), m_efac));
    m_fp.addCover (bind::fapp (fi.sumPred, postArgs), 
                   mk<AND> (mk<OR> (postArgs [0], mk<EQ> (postArgs [1], postArgs [2])),
                            mk<OR> (mk<NEG> (postArgs [0]),
                                    mk<NEG> (postArgs [1]), postArgs [2])));
  }
  
  
  void SmallHornifyFunction::runOnFunction (Function &F)
  {

    const BasicBlock *exit = findExitBlock (F);
    if (!exit)
    {
      errs () << "The exit block of " << F.getName () << " is unreachable.\n";
      return;
    }

    DenseMap<const BasicBlock*, Expr> pred;
    ExprVector sorts;
    
    const LiveSymbols &ls = m_parent.getLiveSybols (F);

    for (auto &BB : F)
    {
      // create predicate for the basic block
      Expr decl = m_parent.bbPredicate (BB);
      // register with fixedpoint
      m_fp.registerRelation (decl);
      
      // -- attempt to extract FunctionInfo record from the current basic block
      // -- only succeeds if the current basic block is the last one
      // -- also constructs summary predicates
      if (m_interproc) extractFunctionInfo (BB);
    }

    BasicBlock &entry = F.getEntryBlock ();
    ExprSet allVars;
    SymStore s(m_efac);
    for (const Expr& v : ls.live (&F.getEntryBlock ())) allVars.insert (s.read (v));
    Expr rule = s.eval (bind::fapp (m_parent.bbPredicate (entry), ls.live (&entry)));
    rule = boolop::limp (boolop::lneg (s.read (m_sem.errorFlag (entry))), rule);
    m_fp.addRule (allVars, rule);
    allVars.clear ();
      
    ExprVector side;
    for (auto &BB : F)
    {
      const BasicBlock *bb = &BB;
      for (const BasicBlock *dst : succs (*bb))
      {
        allVars.clear ();
        s.reset ();
        side.clear ();
        
        const ExprVector &live = ls.live (bb);
        for (const Expr &v : live) allVars.insert (s.read (v));
        
        Expr pre = s.eval (bind::fapp (m_parent.bbPredicate (BB), live));
        side.push_back (boolop::lneg ((s.read (m_sem.errorFlag (BB)))));
        m_sem.execEdg (s, BB, *dst, side);

        Expr tau = mknary<AND> (mk<TRUE> (m_efac), side);

        expr::filter (tau, bind::IsConst(), 
                      std::inserter (allVars, allVars.begin ()));
        for (const Expr &v : ls.live (dst)) allVars.insert (s.read (v));

        Expr post;
        post = s.eval (bind::fapp (m_parent.bbPredicate (*dst), ls.live (dst)));
        
        LOG("seahorn", errs() << "Adding rule : " 
            << *mk<IMPL> (boolop::land (pre, tau), post) << "\n";);
        m_fp.addRule (allVars, boolop::limp (boolop::land (pre, tau), post));
      }
    }

    allVars.clear ();
    side.clear ();
    s.reset ();
    
    
    // Add error flag exit rules
    // bb (err, V) & err -> bb_exit (err , V)
    assert(exit);

    for (auto &BB : F)
    {
      if (&BB == exit) continue;
      
      // XXX Can optimize. Only need the rules for BBs that trip the
      // error flag (directly or indirectly)
      s.reset ();
      allVars.clear ();
      const ExprVector &live = ls.live (&BB);
      for (const Expr &v : live) allVars.insert (s.read (v));
      Expr pre = s.eval (bind::fapp (m_parent.bbPredicate (BB), live));
      pre = boolop::land (pre, s.read (m_sem.errorFlag (BB)));
      
      for (const Expr &v : ls.live (exit)) allVars.insert (s.read (v));
      Expr post = 
        s.eval (bind::fapp (m_parent.bbPredicate (*exit), ls.live (exit)));
      m_fp.addRule (allVars, boolop::limp (pre, post));
    }
    
    if (F.getName ().equals ("main") && ls.live(exit).size () == 1)
      m_fp.addQuery (bind::fapp (m_parent.bbPredicate(*exit), mk<TRUE> (m_efac)));
    else if (F.getName ().equals ("main") && ls.live (exit).size () == 0)
      m_fp.addQuery (bind::fapp (m_parent.bbPredicate(*exit)));
    else if (m_interproc)
    {
      // the summary rule
      // exit(live_at_exit) & !error.flag ->
      //                  summary(true, false, false, regions, arguments, globals, return)
      
      const ExprVector &live = ls.live (exit);
      for (const Expr &v : live) allVars.insert (s.read (v));
      Expr pre = s.eval (bind::fapp (m_parent.bbPredicate (*exit), live));
      pre = boolop::land (pre, boolop::lneg (s.read (m_sem.errorFlag (*exit))));
      
      Expr falseE = mk<FALSE> (m_efac);
      ExprVector postArgs {mk<TRUE> (m_efac), falseE, falseE};
      const FunctionInfo &fi = m_sem.getFunctionInfo (F);
      fi.evalArgs (m_sem, s, std::back_inserter (postArgs));
      std::copy_if (postArgs.begin () + 3, postArgs.end (), 
                    std::inserter (allVars, allVars.begin ()),
                    bind::IsConst());
      Expr post = bind::fapp (fi.sumPred, postArgs);
      m_fp.addRule (allVars, boolop::limp (pre, post));
      
      // the error rule
      // bb_exit (true, V) -> S(true, false, true, V)
      pre = boolop::land (pre->arg (0), s.read (m_sem.errorFlag (*exit)));
      postArgs [2] = mk<TRUE> (m_efac);
      post = bind::fapp (fi.sumPred, postArgs);
      m_fp.addRule (allVars, boolop::limp (pre, post));
    }
    else if (!exit & m_interproc) assert (0);
    
  }

  void LargeHornifyFunction::runOnFunction (Function &F)
  {

    const BasicBlock *exit = findExitBlock (F);
    if (!exit)
    {
      errs () << "The exit block of " << F.getName () << " is unreachable.\n";
      return;
    }


    CutPointGraph &cpg = m_parent.getCpg (F);
    const LiveSymbols &ls = m_parent.getLiveSybols (F);
    
    ExprVector sorts;

    for (const CutPoint &cp : cpg)
    {
      Expr decl = m_parent.bbPredicate (cp.bb ());
      m_fp.registerRelation (decl);
      if (m_interproc) extractFunctionInfo (cp.bb ());
    }
          
    const BasicBlock &entry = F.getEntryBlock ();
    
    ExprSet allVars;
    ExprVector args;
    SymStore s (m_efac);
    for (const Expr& v : ls.live (&entry)) args.push_back (s.read (v));
    allVars.insert (args.begin (), args.end ());
    
    Expr rule = bind::fapp (m_parent.bbPredicate (entry), args);
    rule = boolop::limp (boolop::lneg (s.read (m_sem.errorFlag (entry))), rule);
    m_fp.addRule (allVars, rule);
    allVars.clear ();
    
    UfoLargeSymExec lsem (m_sem);
    
    for (const CutPoint &cp : cpg)
      {
        for (const CpEdge *edge : boost::make_iterator_range (cp.succ_begin (),
                                                              cp.succ_end ()))
        {
          allVars.clear ();
          args.clear ();
          s.reset ();
          const ExprVector &live = ls.live (&cp.bb ());

          for (const Expr &v : live) args.push_back (s.read (v));
          allVars.insert (args.begin (), args.end ());
          
          Expr pre = bind::fapp (m_parent.bbPredicate (cp.bb ()), args);
          
          ExprVector side;
          side.push_back (boolop::lneg ((s.read (m_sem.errorFlag (cp.bb ())))));
          lsem.execCpEdg (s, *edge, side);
          Expr tau = mknary<AND> (mk<TRUE> (m_efac), side);
          expr::filter (tau, bind::IsConst(), 
                        std::inserter (allVars, allVars.begin ()));

          const BasicBlock &dst = edge->target ().bb ();
          args.clear ();
          for (const Expr &v : ls.live (&dst)) args.push_back (s.read (v));
          allVars.insert (args.begin (), args.end ());
          
          Expr post = bind::fapp (m_parent.bbPredicate (dst), args);
          m_fp.addRule (allVars, boolop::limp (boolop::land (pre, tau), post));
        }
      }
    
    allVars.clear ();
    args.clear ();
    s.reset ();
    
    // Add error flag exit rules
    // bb (err, V) & err -> bb_exit (err , V)
    assert(exit);

    for (const CutPoint &cp : cpg)
    {
      if (&cp.bb () == exit) continue;
      
      // XXX Can optimize. Only need the rules for BBs that trip the
      // error flag (directly or indirectly)
      s.reset ();
      allVars.clear ();
      args.clear ();
      
      const ExprVector &live = ls.live (&cp.bb ());
      for (const Expr &v : live) args.push_back (s.read (v));
      allVars.insert (args.begin (), args.end ());
      
      Expr pre = bind::fapp (m_parent.bbPredicate (cp.bb ()), args);
      pre = boolop::land (pre, s.read (m_sem.errorFlag (cp.bb ())));
      
      args.clear ();
      for (const Expr &v : ls.live (exit)) args.push_back (s.read (v));
      allVars.insert (args.begin (), args.end ());
      
      Expr post = bind::fapp (m_parent.bbPredicate (*exit), args);
      m_fp.addRule (allVars, boolop::limp (pre, post));
    }   
    
    if (F.getName ().equals ("main") && ls.live (exit).size () == 1)
      m_fp.addQuery (bind::fapp (m_parent.bbPredicate(*exit), mk<TRUE> (m_efac)));
    else if (F.getName ().equals ("main") && ls.live (exit).size () == 0)
      m_fp.addQuery (bind::fapp (m_parent.bbPredicate(*exit)));
    else if (m_interproc)
    {
      // the summary rule
      // exit(live_at_exit) & !error.flag ->
      //     summary(true, false, false, regions, arguments, globals, return)
      
      args.clear ();
      allVars.clear ();
      
      const ExprVector &live = ls.live (exit);
      for (const Expr &v : live) args.push_back (s.read (v)); 
      allVars.insert (args.begin (), args.end ());
      
      Expr pre = bind::fapp (m_parent.bbPredicate (*exit), args);
      pre = boolop::land (pre, boolop::lneg (s.read (m_sem.errorFlag (*exit))));
      
      Expr falseE = mk<FALSE> (m_efac);
      ExprVector postArgs {mk<TRUE> (m_efac), falseE, falseE};
      const FunctionInfo &fi = m_sem.getFunctionInfo (F);
      fi.evalArgs (m_sem, s, std::back_inserter (postArgs));
      std::copy_if (postArgs.begin () + 3, postArgs.end (), 
                    std::inserter (allVars, allVars.begin ()),
                    bind::IsConst());
      Expr post = bind::fapp (fi.sumPred, postArgs);
      m_fp.addRule (allVars, boolop::limp (pre, post));
      
      // the error rule
      // bb_exit (true, V) -> S(true, false, true, V)
      pre = boolop::land (pre->arg (0), s.read (m_sem.errorFlag (*exit)));
      postArgs [2] = mk<TRUE> (m_efac);
      post = bind::fapp (fi.sumPred, postArgs);
      m_fp.addRule (allVars, boolop::limp (pre, post));
    }
    else if (!exit & m_interproc) assert (0);
  
  }

  bool HornifyFunction::checkProperty(ExprVector predicates, Expr &cex){
    tribool res  = m_fp.query();
    std::vector<std::string> invariants;
    if (res){
      errs() << "UNSAFE";
      LOG("seahorn", errs() << "\nCEX\n");
      //cex = m_fp.getCex();
      LOG("seahorn", errs() << "\t" << m_zctx.toSmtLib(cex) << "\n");

      return false;
    }else if (!res){
      errs() << "SAFE";
      LOG("seahorn", errs() << "\nINVARIANTS");
      for (Expr p: predicates){
        Expr delta = m_fp.getCoverDelta(p);
        if (!(isOpX<TRUE> (delta))) {
          LOG("seahorn", errs() << "\t- BLOCK : " << m_zctx.toSmtLib(p) << "\n");
          std::string inv = m_zctx.toSmtLib(delta);
          invariants.push_back (inv);
          LOG("seahorn", errs() << "\t\t" << inv << "\n");
        }
      }
      return true;
    }else{
      errs() <<"ERROR\n";
      exit (1);
    }
  }
  

}
