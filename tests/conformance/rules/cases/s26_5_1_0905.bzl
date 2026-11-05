//! case: s26_5_1_0905
//! doc: spec
//! section: 26.5.1
//! title: user extensions with differing parameter types are chosen exactly when the argument matches
//! rule: extension members of the called name that match the call's arity and argument types
//! cites: 26.5.1
//! expect: run
//! output: 77
//!   55
//!   9
//!   3
//!   12
//!   9
module Conf;

extension Object
  public method Select(s: string): int => 77;
  public method OrderBy(n: int): int => 55;
  public method Max(s: string): int => 9;
  public method Any(s: string): int => 3;
end;

public class Program
  static method S(): string => "q";
  static method N(): int => 3;

  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    a.Add(3);
    Console.WriteLine(a.Select(S()));
    Console.WriteLine(a.OrderBy(N()));
    Console.WriteLine(a.Max(S() + "z"));
    Console.WriteLine(a.Any($"{N()}"));
    Console.WriteLine(a.Select(lambda(x: int): int => x * 2).Sum());
    Console.WriteLine(a.Max(lambda(x: int): int => x * 3));
  end Main;
end Program;
