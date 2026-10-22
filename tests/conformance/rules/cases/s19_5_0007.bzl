//! case: s19_5_0007
//! doc: spec
//! section: 19.5
//! title: an untyped var slot binds null, a typed Object slot does not, and an unmatched case
//!   raises
//! rule: "A typed binding pattern `var n: T` also matches only non-null values — the type
//!   annotation makes it a type-constrained binding, not an unconditional binding. `null` is
//!   matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind
//!   any value including null), or by `_`." (§19.5, v0.340) `Pair(var a, var n)` binds any
//!   slot value including `null`, so it is irrefutable; `Pair(var a: Object, var n)` does
//!   not match a `null` slot, and an expression `case` with no matching arm raises
//!   `MatchFailureException` (§19.6).
//! cites: 19.6, 19.3
//! expect: run
//! output: untyped
//!   bind
//!   mfe
module Conf;

public record Pair(A: Object?, B: int);

public class Program
  public static method T(pr: Pair): string
    return (case pr of
      Pair(var a: Object, var n) => "bind",
      Pair(var a, var n) => "untyped");
  end T;

  public static method U(pr: Pair): string
    return (case pr of Pair(var a: Object, var n) => "bind");
  end U;

  public static method Main()
    Console.WriteLine(T(new Pair(null, 3)));
    Console.WriteLine(T(new Pair("s", 3)));
    try
      Console.WriteLine(U(new Pair(null, 3)));
    catch MatchFailureException =>
      Console.WriteLine("mfe");
    end try;
  end Main;
end Program;
