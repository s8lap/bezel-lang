//! case: s22_6_0087
//! doc: spec
//! section: 22.6
//! title: a direct call with an explicit list picks the only applicable overload of a generic class
//! rule: Overload ranking considers only applicable candidates (22.6); with the explicit list binding V := int, the Array<Array<V>> overload is the only one an Array<Array<int>> argument matches.
//! cites: 22.6, 14.5
//! expect: run
//! stdout: 20 10
//! issue: 6228
module Conf;

public interface I
  method S<U>(x: Array<Array<U>>): int;
  method S<U>(x: Array<U>): int;
end I;

public class C: I
  public constructor() end constructor;
  public method S<V>(x: Array<V>): int => 10;
  public method S<V>(x: Array<Array<V>>): int => 20;
end C;

public class Program
  public static method Main()
    using var c := new C();
    using var a := new Array<int>();
    using var b := new Array<Array<int>>();
    Console.WriteLine($"{c.S<int>(b)} {c.S<int>(a)}");
  end Main;
end Program;
