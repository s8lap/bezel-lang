//! case: s7_1_0010
//! doc: spec
//! section: 7.1
//! title: a nullable int argument at Math.Min with an explicit type argument int is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Explicit type argument: T is written int, so parameter a is int and d: int? does not convert.
module Conf;

public class Program
  public static method Pick(d: int?): int
    return Math.Min<int>(d, 2);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
