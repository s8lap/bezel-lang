//! case: s11_2_0161
//! doc: spec
//! section: 11.2
//! title: an extra class overload does not displace the interface member with the exact parameter type
//! rule: parameter types that are exactly equal under the interface's substitution (v0.338 §11.2)
//! cites: 11.2
//! expect: run
//! output: 1 2
module Conf;

public interface I
  method F(x: int): int;
end I;

public class C: I
  public constructor() end constructor;
  public method F(x: int): int => 1;
  public method F(x: long): int => 2;
end C;

public class Program
  public static method Main()
    using var c := new C();
    var i: I := c;
    var big: long := 5;
    Console.WriteLine(i.F(5).ToString() + " " + c.F(big).ToString());
  end Main;
end Program;
