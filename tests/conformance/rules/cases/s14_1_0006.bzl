//! case: s14_1_0006
//! doc: spec
//! section: 14.1
//! title: generic methods returning a two-parameter generic choice keep case and payload at every placement
//! rule: "For a **generic** choice the type arguments are written at the construction (`Result<int, string>.Ok(value)`)" (§35.3, v0.340); a generic member is evaluated at each instantiation after substituting the type arguments (§3.6 rule 2, §14.1), so the payload constructed in the generic body is the payload matched by the caller. Each method builds `E<..>.L` or `E<..>.R` at its own instantiation; with `T = int` the results are `L1`, `R2`, `R40`, `L41`, `R5`, `RTrue` and `R7`, whichever of the two type parameters the generic argument stands for and whether the payload is `T`, a constant of another type, or `T` repeated.
//! cites: 35.3, 3.6
//! expect: run
//! output: L1
//!   R2
//!   R40
//!   L41
//!   R5
//!   RTrue
//!   R7
module Conf;

public choice E<A, B>
  L(a: A),
  R(b: B),
  Z;
end E;

public class Api
  public static method F<T>(x: T): E<T, int> => E<T, int>.L(x);
  public static method G<T>(x: T): E<int, T> => E<int, T>.R(x);
  public static method H<T>(x: T): E<T, int> => E<T, int>.R(40);
  public static method J<T>(x: T): E<int, T> => E<int, T>.L(41);
  public static method K<T>(x: T): E<T, T> => E<T, T>.R(x);
  public static method M<T>(x: T): E<T, bool> => E<T, bool>.R(true);
  public static method Q<T>(x: T): E<bool, T> => E<bool, T>.R(x);
end Api;

public class Program
  public static method Main()
    Console.WriteLine((case Api.F<int>(1) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.G<int>(2) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.H<int>(3) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.J<int>(4) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.K<int>(5) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.M<int>(6) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
    Console.WriteLine((case Api.Q<int>(7) of L(var a) => $"L{a}", R(var b) => $"R{b}", Z => "Z"));
  end Main;
end Program;
