//! case: l2_3_0008
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString refuses a string argument
//! rule: stdlib §2.3 declares `ToString` for `int`, `long`, `float`, `double`,
//!     `decimal` and `bool` - there is no `string` overload, the `To*` parsing
//!     forms take the `string` and the rendering forms do not - and §5.1's table
//!     admits no conversion from a `string` to any of them, so no candidate of
//!     §22.6 is applicable and the call is a compile error.
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: must be an `int`, a `long`, a `float`, a `double`, a `decimal` or a `bool`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString("abc"));
  end Main;
end Program;
