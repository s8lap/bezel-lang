//! case: s19_5_0006
//! doc: spec
//! section: 19.5
//! title: a typed slot binding in a generic record matches the substituted type and declines
//!   null
//! rule: "A typed binding pattern `var n: T` also matches only non-null values — the type
//!   annotation makes it a type-constrained binding, not an unconditional binding. `null` is
//!   matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind
//!   any value including null), or by `_`." (§19.5, v0.340) Over `Box<int>` the slot is a
//!   present `int` and `var a: int` always matches; over `Box<int?>` the slot holds `null`
//!   and the typed binding does not match, so the wildcard arm is reached.
//! cites: 19.3, 14.1
//! expect: run
//! output: int0
//!   int7
//!   strq
//!   none
//!   int
module Conf;

public record Box<T>(X: T, Y: int);

public class Program
  public static method I(b: Box<int>): string
    return (case b of
      Box(var a: int, var n) => "int" + a.ToString(),
      _ => "none");
  end I;

  public static method S(b: Box<string>): string
    return (case b of
      Box(var a: string, var n) => "str" + a,
      _ => "none");
  end S;

  public static method IN(b: Box<int?>): string
    return (case b of
      Box(var a: int, var n) => "int",
      _ => "none");
  end IN;

  public static method Main()
    Console.WriteLine(I(new Box<int>(0, 1)));
    Console.WriteLine(I(new Box<int>(7, 1)));
    Console.WriteLine(S(new Box<string>("q", 1)));
    Console.WriteLine(IN(new Box<int?>(null, 1)));
    Console.WriteLine(IN(new Box<int?>(4, 1)));
  end Main;
end Program;
