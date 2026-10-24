//! case: s28_3_0214
//! doc: spec
//! section: 28.3
//! title: a self-assignment to an unclassified local is admitted
//! rule: An unclassified local that is never classified stays outside the
//!   ownership rules, as a `string` local does: nothing disposes it and its
//!   reads borrow from nothing.
//! cites: 28.3, 8.6
//! expect: accept
//! issue: 6609
module Conf;

public class Program
  public method Demo()
    var o: Object := 7;   // a boxing conversion — a runtime-managed production
    o := o;               // a read of an unclassified local is no borrow
  end Demo;
end Program;
