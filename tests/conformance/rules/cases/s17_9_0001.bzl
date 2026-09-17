//! case: s17_9_0001
//! doc: spec
//! section: 17.9
//! title: nameof of a local returns its source-level identifier
//! rule: "`nameof(expr)` returns the source-level name of an identifier as a
//!   compile-time `string` constant." Example: `nameof(amount) // "amount"`.
//! cites: 17.9
//! expect: run
//! stdout: amount
module Conf;

public class Program
  public static method Main()
    var amount := 5;
    Console.WriteLine(nameof(amount));
  end Main;
end Program;
