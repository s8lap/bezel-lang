//! case: l3_1_0049
//! doc: stdlib
//! section: 3.1
//! title: a char argument to a StringBuilder member's string parameter is rejected
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string): StringBuilder`.
//!   §5.1's implicit-conversion table lists a `char` as converting to `int`,
//!   `uint`, `long`, `ulong`, `float`, `double` and `decimal` - never to `string` -
//!   so no candidate of §22.6 is applicable and the call is a compile error.
//! cites: 3.1, 5.1, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    _ := sb.Replace('l', 'L');
  end Main;
end Program;
