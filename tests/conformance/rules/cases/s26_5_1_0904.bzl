//! case: s26_5_1_0904
//! doc: spec
//! section: 26.5.1
//! title: a conditional whose arms are a string call and a string concatenation selects the Object-headed
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: run
//! output: 4242
module Conf;

extension Object
  public method Where(s: string): int => 4242;
end;

public class Program
  static method S(): string => "q";
  static method F(): bool => false;

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Where((if F() then S() else "lit" + S())));
  end Main;
end Program;
