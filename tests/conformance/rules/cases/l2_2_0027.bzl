//! case: l2_2_0027
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs refuses a string argument
//! rule: stdlib §2.2 declares `Abs` for `int`, `long`, `double` and `decimal`. §5.1's
//!     implicit-conversion table admits no conversion to a `string` (and none from
//!     a `string` to a numeric type), so no candidate of §22.6 is applicable and
//!     the call is a compile error.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: must be an `int`, a `long`, a `double` or a `decimal`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Abs("x"));
  end Main;
end Program;
