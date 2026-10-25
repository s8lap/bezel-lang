//! case: l3_1_0050
//! doc: stdlib
//! section: 3.1
//! title: an int argument to a StringBuilder member's string parameter is rejected
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string): StringBuilder`.
//!   §5.1's implicit-conversion table lists an `int` as converting to `long`,
//!   `float`, `double` and `decimal` - never to `string` - so no candidate of
//!   §22.6 is applicable and the call is a compile error.
//! cites: 3.1, 5.1, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    _ := sb.Replace(3, 4);
  end Main;
end Program;
