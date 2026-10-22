//! case: s19_5_0005
//! doc: spec
//! section: 19.5
//! title: a typed slot binding over a nullable slot declines null before its guard is evaluated
//! rule: "A typed binding pattern `var n: T` also matches only non-null values — the type
//!   annotation makes it a type-constrained binding, not an unconditional binding. `null` is
//!   matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind
//!   any value including null), or by `_`." (§19.5, v0.340) The typed arm does not match a
//!   `null` slot, so its `when` guard (§19.4), which would read `a.Length` on a `null`, is
//!   never evaluated and the `null` arm is reached.
//! cites: 19.4, 19.3
//! expect: run
//! output: nullA
//!   short
//!   long
module Conf;

public record Pair(A: string?, B: int);

public class Program
  public static method T(pr: Pair): string
    return (case pr of
      Pair(var a: string, var n) when a.Length > 1 => "long",
      Pair(var a: string, var n) => "short",
      Pair(null, var n) => "nullA",
      _ => "x");
  end T;

  public static method Main()
    Console.WriteLine(T(new Pair(null, 3)));
    Console.WriteLine(T(new Pair("a", 3)));
    Console.WriteLine(T(new Pair("ab", 3)));
  end Main;
end Program;
