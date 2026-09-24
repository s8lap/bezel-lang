//! case: s28_3_0006
//! doc: spec
//! section: 28.3
//! title: assigning a borrow into an owned-state local is rejected
//! rule: State match on reassignment: an owned-state LHS requires an owned RHS;
//!   assigning a borrow to an owned-state LHS is rejected.
//! expect: reject
//! diagnostic: owned-state
//! issue: 4842, 4846
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var best := new Widget();
    var other := new Widget();
    var next := other;       // next is a borrow of OTHER (cross-source); the borrow's
                              // source is `other`, not `best`, so the consume-site
                              // freeze on `best` is silent here
    best := next;            // assigning cross-source borrow to owned-state LHS — REJECT
  end Demo;
end Program;
