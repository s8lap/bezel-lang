//! case: s14_1_0007
//! doc: spec
//! section: 14.1
//! title: generic methods returning a generic choice carry long, string and double payloads
//! rule: "For a **generic** choice the type arguments are written at the construction (`Result<int, string>.Ok(value)`)" (§35.3, v0.340); a generic member is evaluated at each instantiation after substituting the type arguments (§3.6 rule 2, §14.1), so the payload constructed in the generic body is the payload matched by the caller. The payloads are `long` 5000000000 (beyond `int`), the `long` 7 given as `T = int` 2, a `string`, and a `double` 2.5, in each of the two payload positions of `E<A, B>`.
//! cites: 35.3, 3.6
//! expect: run
//! output: R5000000000
//!   R2
//!   Rhi
//!   L4
//!   L2.5
//!   L6
module Conf;

public choice E<A, B>
  L(a: A),
  R(b: B),
  Z;
end E;

public class Api
  public static method K<T>(x: T, y: long): E<T, long> => E<T, long>.R(y);
  public static method K2<T>(x: T, y: long): E<long, T> => E<long, T>.R(x);
  public static method S<T>(x: T, s: string): E<T, string> => E<T, string>.R(s);
  public static method S2<T>(x: T, s: string): E<T, string> => E<T, string>.L(x);
  public static method D<T>(x: T, d: double): E<double, T> => E<double, T>.L(d);
  public static method KK<T>(x: T): E<T, T> => E<T, T>.L(x);
end Api;

public class Program
  public static method Main()
    Console.WriteLine((case Api.K<int>(1, 5000000000) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.K2<int>(2, 7) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.S<int>(3, "hi") of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.S2<int>(4, "hi") of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.D<int>(5, 2.5) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.KK<int>(6) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
  end Main;
end Program;
