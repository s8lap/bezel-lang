//! case: s17_5_0015
//! doc: spec
//! section: 17.5
//! title: an int left operand boxes to Object and is found in an Array<Object>
//! rule: "any type to `Object`: ... value types (primitives, records, enums, ...) by boxing" is an implicit conversion ([§5.1]), so the left operand is admissible as `Contains(item: Object)`'s argument ([§17.5]); a primitive's default equality is value comparison ([§8.3]).
//! cites: 17.5, 5.1, 8.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<Object> := ["a", 1];
    var i: int := 1;
    var found := i in a;
    Console.WriteLine(found);
  end Main;
end Program;
