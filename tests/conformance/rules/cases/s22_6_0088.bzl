//! case: s22_6_0088
//! doc: spec
//! section: 22.6
//! title: an interface call with an explicit list ranks a function-typed parameter by its slot
//! rule: Overload ranking considers only applicable candidates (22.6). With the explicit list binding U := int, a lambda of type (int) -> int is applicable only to the overload whose function slot returns int.
//! cites: 22.6, 14.5, 11.2
//! expect: run
//! stdout: 20
//! issue: 6228
module Conf;

public interface I
  method T<U>(x: (U) -> int): int;
  method T<U>(x: (U) -> string): int;
end I;

public class C: I
  public constructor() end constructor;
  public method T<V>(x: (V) -> string): int => 10;
  public method T<V>(x: (V) -> int): int => 20;
end C;

public class Program
  public static method Main()
    using var c := new C();
    var i: I := c;
    Console.WriteLine(i.T<int>(lambda(n: int): int => n));
  end Main;
end Program;
