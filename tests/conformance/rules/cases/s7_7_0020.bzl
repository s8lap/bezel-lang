//! case: s7_7_0020
//! doc: spec
//! section: 7.7
//! title: a null-coalescing default makes a nullable int admissible at Math.Max
//! rule: the result type is `T` when the right operand is non-nullable and `T?` when it is nullable
//! cites: 2.2
//! expect: run
//! output: 7
//!   9
//! issue: 7660
module Conf;

public class Program
  public static method IdI(x: int?): int? return x; end IdI;

  public static method Main()
    Console.WriteLine(Math.Max(IdI(null) ?? 7, 5));
    Console.WriteLine(Math.Max(IdI(9) ?? 7, 5));
  end Main;
end Program;
