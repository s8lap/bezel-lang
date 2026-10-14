//! case: s22_6_0086
//! doc: spec
//! section: 22.6
//! title: an interface call with an explicit list binds each overload to its exact-typed witness
//! rule: A call through an interface selects among the interface's overloads by the explicit list and the argument types, and runs the witness that implements the selected overload (11.2).
//! cites: 22.6, 14.5, 11.2
//! expect: run
//! stdout: 20 10
//! issue: 6228
module Conf;

public interface I
  method S<U>(x: Array<U>): int;
  method S<U>(x: LinkedList<U>): int;
end I;

public class C: I
  public constructor() end constructor;
  public method S<V>(x: LinkedList<V>): int => 10;
  public method S<V>(x: Array<V>): int => 20;
end C;

public class Program
  public static method Main()
    using var c := new C();
    var i: I := c;
    using var a := new Array<int>();
    using var l := new LinkedList<int>();
    Console.WriteLine($"{i.S<int>(a)} {i.S<int>(l)}");
  end Main;
end Program;
