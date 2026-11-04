//! case: s7_1_0006
//! doc: spec
//! section: 7.1
//! title: a nullable int argument at Math.Max's second position is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Second-position twin of the first-position row; stdlib 2.2 declares Max<T>(a: T, b: T).
module Conf;

public class Program
  public static method Pick(d: int?): int
    return Math.Max(5, d);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
