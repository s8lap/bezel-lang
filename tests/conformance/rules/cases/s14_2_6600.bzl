//! case: s14_2_6600
//! doc: spec
//! section: 14.2
//! title: stdlib record `Guid` does not satisfy `where T: class` at a method call
//! rule: Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types.
//! cites: 14.2
//! expect: reject
//! diagnostic: class
//! issue: 8591
module Conf;
public class Program
  public static method Main(): int
    return F(Guid.NewGuid());
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
