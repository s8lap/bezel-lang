//! case: s7_4_0911
//! doc: spec
//! section: 7.4
//! title: a null test composed with and narrows the right operand of a ReadOnlyStringSpan? parameter
//! rule: in `A and B`, `B` (and the true branch) is checked under `A`'s true-facts
//! cites: 7.4
//! expect: run
//! output: True
//!   False
//!   False
module Conf;

public class Program
  static method F(s: ReadOnlyStringSpan?): bool => s is not null and s.Length > 2 and s[0] = 'e';

  public static method Main()
    var w := "hello";
    Console.WriteLine(F(new ReadOnlyStringSpan(w, 1, 3)));
    Console.WriteLine(F(new ReadOnlyStringSpan(w, 0, 3)));
    Console.WriteLine(F(null));
  end Main;
end Program;
