//! case: s8_5_1101
//! doc: spec
//! section: 8.5
//! title: an interpolation hole of an abstract class type dispatches to the concrete override
//! rule: `ToString` is invoked implicitly inside string interpolation ... via virtual dispatch
//! cites: 18.5
//! expect: run
//! output: A1(7)
//! issue: 8142
module Conf;

public abstract class A
  public abstract method Foo(): int;
end A;

public class A1: A
  public N: int := 7;

  public override method Foo(): int => 1;

  public override method ToString(): string => $"A1({N})";
end A1;

public class Program
  public static method Main()
    var a: A := new A1();
    Console.WriteLine($"{a}");
  end Main;
end Program;
