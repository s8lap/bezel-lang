//! case: s7_1_0005
//! doc: spec
//! section: 7.1
//! title: a nullable int argument at Math.Min's first position is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: stdlib 2.2 declares Min<T>(a: T, b: T) where T: IComparable<T>; the 14.2 bound admits only non-nullable substitutions, so d: int? cannot bind a: T. The accepting twins are l2_2_0030 and s7_4_0155 (narrowed operands).
module Conf;

public class Program
  public static method Pick(d: int?): int
    return Math.Min(d, 5);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
