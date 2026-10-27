//! case: l2_3_0014
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a ulong value with a toBase reaches no candidate
//! rule: stdlib §2.3 declares the two-argument forms as `ToString(value: int,
//!     toBase: int)` and `ToString(value: long, toBase: int)`. A `ulong` reaches
//!     neither `int` nor `long` (§5.1: "Between signed and unsigned types of the
//!     same or narrower width ... is never implicit", and the `ulong` → `long`
//!     widening is not among the admitted ones), so no candidate is applicable
//!     at the value position and §22.6's rule applies: "If no candidate is
//!     applicable at all ... the call is a compile error with no candidates
//!     cited".
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  public static method Main()
    var u: ulong := 5UL;
    Console.WriteLine(Convert.ToString(u, 10));
  end Main;
end Program;
