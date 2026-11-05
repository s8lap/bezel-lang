//! case: s26_5_1_0908
//! doc: spec
//! section: 26.5.1
//! title: an int argument matches neither the LINQ Where(predicate) nor the Object-headed Where(string)
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: reject
//! diagnostic: Where
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  static method N(): int => 3;

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Where(N()));
  end Main;
end Program;
