//! case: l3_1_0055
//! doc: stdlib
//! section: 3.1
//! title: a runtime long argument to a StringBuilder member's int parameter is rejected
//! rule: stdlib §3.1 declares `Insert(index: int, value: string): StringBuilder`.
//!   §5.1's implicit-conversion table admits a `long` only to `float`, `double`
//!   and `decimal`, and its constant narrowing reaches a narrower integer type
//!   only for a compile-time constant - a run-time integer value never narrows
//!   implicitly. No candidate of §22.6 is applicable and the call is a compile
//!   error.
//! cites: 3.1, 5.1, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    var i: long := 1;
    _ := sb.Insert(i, "X");
  end Main;
end Program;
