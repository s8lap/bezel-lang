//! case: s14_6_0003
//! doc: spec
//! section: 14.6
//! title: an expanding cycle whose halves two sibling modules' implementations hold is rejected
//! rule: Cycles spanning modules are checked over the compilation closure
//!   §2.8.3 rule 1 supplies, and the graph is the **program's**: program
//!   assembly (§2.8) re-checks the per-edge condition over the union of every
//!   artifact's edges, so a cycle that no single compilation's closure
//!   holds — two implementations in sibling modules, each contributing one
//!   half of an expanding cycle between a third module's interface methods —
//!   is rejected at assembly
//! cites: 14.6, 2.8.3, 14.2
//! expect: reject
//! diagnostic: cycle
//! issue: 5815
module Conf;
import Conf.Visit;
import Conf.Lhs;
import Conf.Rhs;

public class Program
  public static method Main()
    using var l := new LeftVisitor();
    using var r := new RightVisitor();
    Console.WriteLine(l.M<int>(r));
  end Main;
end Program;
