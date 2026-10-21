//! case: s17_5_0010
//! doc: spec
//! section: 17.5
//! title: a char tests membership in a string literal, the spec's own example
//! rule: "`in` produces `bool`." and the example `var hasChar := 'a' in
//!   "abcdef";`. The right operand must expose a `Contains(T): bool` method,
//!   and the lookup is structural ([§17.5]).
//! cites: 17.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var hasChar := 'a' in "abcdef";
    Console.WriteLine(hasChar);
  end Main;
end Program;
