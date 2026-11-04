//! case: s7_1_0007
//! doc: spec
//! section: 7.1
//! title: a nullable int argument at Math.Clamp's third position is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Third-position row; stdlib 2.2 declares Clamp<T>(value: T, low: T, high: T).
module Conf;

public class Program
  public static method Pick(d: int?): int
    return Math.Clamp(5, 1, d);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
