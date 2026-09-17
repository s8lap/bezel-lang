//! case: s12_7_0004
//! doc: spec
//! section: 12.7
//! title: a bare-binding compound assignment on a narrowed Object is not a box write
//! rule: What is not rejected is the bare-binding compound and ++/-- form:
//!   o += 1 and o++ are equivalent to o := o + 1, and that store targets the
//!   declared Object/interface location -- it boxes the result afresh.
//! cites: 12.7
//! expect: accept
module Conf;

public class Program
  public static method Test()
    var n: int := 5;
    var o: Object := n;
    if o is int then
      o += 1;
    end if;
  end Test;
end Program;
