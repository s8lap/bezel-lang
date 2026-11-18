//! case: s14_2_6609
//! doc: spec
//! section: 14.2
//! title: `string` satisfies `where T: class`
//! rule: Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types.
//! cites: 14.2
//! expect: run
//! output: accepted
//! issue: 8591
module Conf;
public class Program
  public static method Main(): int
    return F("hello");
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
