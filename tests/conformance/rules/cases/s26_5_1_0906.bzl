//! case: s26_5_1_0906
//! doc: spec
//! section: 26.5.1
//! title: a call returning a function value still selects the LINQ Where(predicate) beside an Object-
//!   headed
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: run
//! output: 2
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  static method P(): (int) -> bool => lambda(x: int): bool => x > 1;

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Where(P()).Count());
  end Main;
end Program;
