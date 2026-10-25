//! case: l3_1_0057
//! doc: stdlib
//! section: 3.1
//! title: a constant long argument to a StringBuilder member's int parameter narrows and is admitted
//! rule: stdlib §3.1 declares `Insert(index: int, value: string): StringBuilder`.
//!   §5.1's constant narrowing: an expression of integer type that is a
//!   compile-time constant implicitly converts to any integer type whose range
//!   contains its value, across signedness - so the constant `1L` reaches an
//!   `int` parameter. This is the accepting counterpart of the run-time `long`
//!   argument, which §5.1's conversion table does not reach.
//! cites: 3.1, 5.1
//! expect: run
//! stdout: hXello
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    sb.Insert(1L, "X");
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
