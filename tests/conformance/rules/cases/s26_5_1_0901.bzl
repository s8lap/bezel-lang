//! case: s26_5_1_0901
//! doc: spec
//! section: 26.5.1
//! title: an interpolated string argument selects Object-headed Where(string) over LINQ Where
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: run
//! output: 4242
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    var k := 7;
    Console.WriteLine(a.Where($"x{k}"));
  end Main;
end Program;
