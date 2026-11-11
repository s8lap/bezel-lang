//! case: s28_10_4601
//! doc: spec
//! section: 28.10
//! title: an unmarked using var over a collection-literal local of a class-typed collection is ambiguous
//! rule: An unmarked `using var y := x;` is a **compile error** when its right-hand side is a named owned binding whose declared type, nullability stripped, is a class or interface type
//! cites: 28.3.4, 15.4
//! expect: reject
//! diagnostic: unmarked `using var b := a;`
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var b: Array<int> := a;
    Console.WriteLine(b.Count);
  end Main;
end Program;
