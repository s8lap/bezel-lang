//! case: l2_2_0026
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs refuses a bool argument
//! rule: stdlib §2.2 declares `Abs` for `int`, `long`, `double` and `decimal`. §5.1's
//!     never-implicit list bars the only conversion that could reach one of them:
//!     "Between integers and `bool`". So no candidate of §22.6 is applicable and
//!     the call is a compile error.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: must be an `int`, a `long`, a `double` or a `decimal`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Abs(true));
  end Main;
end Program;
