//! case: s27_1_1_0005
//! doc: spec
//! section: 27.1.1
//! title: a function value is never usable at a function type with a different parameter count
//! rule: Every rule of function-type assignability is slot-wise over equal
//!   parameter counts — a value is never usable at a function type with a
//!   different number of parameter slots, whatever the slots' types (v0.296).
//! cites: 27.1.1
//! expect: reject
//! diagnostic: parameter slots
module Conf;

public class Program
  public method Demo()
    var one: (int) -> int := lambda(x: int) => x;
    var two: (int, int) -> int := one;
  end Demo;
end Program;
