//! case: s14_2_6610
//! doc: spec
//! section: 14.2
//! title: an `Array` of a stdlib record is a heap collection and satisfies `where T: class`
//! rule: Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types.
//! cites: 14.2
//! expect: run
//! output: accepted
//! issue: 8591
module Conf;
public class Program
  public static method Main(): int
    using var a := new Array<TimeSpan>(2);
    return F(a);
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
