//! case: s28_3_0006
//! doc: spec
//! section: 28.3
//! title: assigning a borrow into an owned-state local is rejected
//! rule: State match on reassignment: an owned-state LHS requires an owned RHS;
//!   assigning a borrow to an owned-state LHS is rejected.
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var best := new Widget();
    var next := new Widget();
    best := next;
  end Demo;
end Program;
