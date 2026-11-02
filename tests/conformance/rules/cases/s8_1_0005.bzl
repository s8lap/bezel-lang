//! case: s8_1_0005
//! doc: spec
//! section: 8.1
//! title: ReferenceEquals compares two task values by identity
//! rule: `Object.ReferenceEquals(a, b)` compares object identity, ignoring
//!   any `Equals` override.
//! cites: 8.1, 8.4, 30.3
//! expect: run
//! output: True
//!   False
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    using var u := Task.FromResult(2);
    var b := Object.ReferenceEquals(t, t);
    var c := Object.ReferenceEquals(t, u);
    Console.WriteLine(b.ToString());
    Console.WriteLine(c.ToString());
    return 0;
  end Main;
end Program;
