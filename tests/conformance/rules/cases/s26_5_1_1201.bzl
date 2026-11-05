//! case: s26_5_1_1201
//! doc: spec
//! section: 26.5.1
//! title:a string argument selects the Object-headed Where(string) extension on an Array of int
//! rule:extension members of the called name that match the call's arity and argument types
//! expect: run
//! stdout: 4242
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  static method N(): string => "s";

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Where(N()));
  end Main;
end Program;
