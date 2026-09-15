//! case: s5_2_0031
//! doc: spec
//! section: 5.2
//! title: a constant operand the conversion would reject is a compile error at the site
//! rule: Where the operand of an explicit conversion is a compile-time constant
//!   whose value would raise `OverflowException` under any rule of this section
//!   — integer narrowing, the same-width signedness change, the integer-to-`char`
//!   code-point range, and the floating-point and `decimal` conversions — the
//!   conversion is a **compile error at the conversion site**, never a deferred
//!   runtime fault: `uint(-1)` is rejected where it is written.
//! cites: 5.2, 17.14, 5.1
//! expect: reject
//! diagnostic: constant
//! issue: 4405
module Conf;

public class Program
  public method Run(): uint
    return uint(-1);
  end Run;
end Program;
