//! case: s28_3_0215
//! doc: spec
//! section: 28.3
//! title: a self-assignment to an unclassified local holding a string is admitted
//! rule: A runtime-managed RHS — a box, a `string`, a function value, a `Type` —
//!   carries no classification and is outside the match: it displaces as the
//!   LHS's state prescribes and leaves the local unclassified.
//! cites: 28.3, 28.9
//! expect: accept
//! issue: 6609
module Conf;

public class Program
  public method Demo()
    var o: Object := "s";  // a runtime-managed production
    o := o;                // a read of an unclassified local is no borrow
  end Demo;
end Program;
