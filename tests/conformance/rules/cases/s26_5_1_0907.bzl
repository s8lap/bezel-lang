//! case: s26_5_1_0907
//! doc: spec
//! section: 26.5.1
//! title: a conditional of two lambdas still selects the LINQ Where(predicate) beside an Object-headed
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: run
//! output: 2
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  static method F(): bool => true;

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Where((if F() then lambda(x: int): bool => x > 1 else lambda(x: int): bool => x > 2)).Count());
  end Main;
end Program;
