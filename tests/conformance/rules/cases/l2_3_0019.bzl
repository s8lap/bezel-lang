//! case: l2_3_0019
//! doc: stdlib
//! section: 2.3
//! title: an un-narrowed nullable toBase reaches no Convert.ToString candidate
//! rule: stdlib §2.3 declares `ToString(value: int, toBase: int)` and
//!     `ToString(value: long, toBase: int)`. §5.1's nullable conversions lift
//!     an implicit conversion to the nullable forms only — "`S?` implicitly
//!     converts to `T?` whenever `S` implicitly converts to `T`" — so an
//!     `int?` converts to neither `int` nor `long`, and §22.6's rule applies:
//!     "If no candidate is applicable at all ... the call is a compile error
//!     with no candidates cited". The local is never narrowed: its initializer
//!     is a nullable-typed expression, which §7.4 does not narrow.
//! cites: 2.3, 5.1, 7.4, 22.6
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  public static method Radix(): int?
    return 16;
  end Radix;

  public static method Main()
    var r: int? := Radix();
    Console.WriteLine(Convert.ToString(255, r));
  end Main;
end Program;
