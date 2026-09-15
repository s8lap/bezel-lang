//! case: s17_1_0002
//! doc: spec
//! section: 17.1
//! title: additive operators are left-associative
//! rule: Precedence table, Additive row: `+` `-`, associativity left-to-right —
//!   `a - b - c` groups as `(a - b) - c`.
//! cites: 17.1
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var r := 10 - 3 - 2;
    Console.WriteLine(r);
  end Main;
end Program;
