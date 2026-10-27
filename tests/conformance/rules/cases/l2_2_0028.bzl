//! case: l2_2_0028
//! doc: stdlib
//! section: 2.2
//! title: Math.Pow refuses a string argument at its double parameter
//! rule: stdlib §2.2 declares `Pow(x: double, y: double): double`. §5.1's
//!     implicit-conversion table admits no conversion from a `string` to a `double`,
//!     so no candidate of §22.6 is applicable and the call is a compile error.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: must be a `double`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Pow("not a double", 3));
  end Main;
end Program;
