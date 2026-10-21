//! case: s17_5_0033
//! doc: spec
//! section: 17.5
//! title: an Exception left operand is rejected for membership in an Array<StringBuilder>
//! rule: "**class to base class**, transitively, up to `Object`" is the only
//!   class-to-class implicit conversion ([§5.1]); `Exception` is not a base of
//!   `StringBuilder`, so the left operand is not admissible as
//!   `Contains(item: StringBuilder)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
//! issue: 6684
module Conf;

public class Program
  public static method Main()
    var a: Array<StringBuilder> := [new StringBuilder()];
    using var b := new Exception("x");
    var found := b in a;
    Console.WriteLine(found);
  end Main;
end Program;
