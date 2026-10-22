//! case: s19_5_0004
//! doc: spec
//! section: 19.5
//! title: a typed binding in a choice payload slot of nullable type matches only a present value
//! rule: "A typed binding pattern `var n: T` also matches only non-null values — the type
//!   annotation makes it a type-constrained binding, not an unconditional binding. `null` is
//!   matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind
//!   any value including null), or by `_`." (§19.5, v0.340) The rule holds at a
//!   destructuring slot: `Mark(var v: int, var w: string)` matches only when both `int?` and
//!   `string?` payloads are present, while the untyped `var` slot accepts `null`.
//! cites: 19.3, 35.5
//! expect: run
//! output: circ0
//!   markany
//!   markv
//!   markw
//!   markboth
//!   cl
//!   none
module Conf;

public record Pt(X: int, Y: int);

public choice Sh
  Circ(R: int),
  Mark(V: int?, W: string?),
  Cl(P: Pt),
  None;
end Sh;

public class Program
  public static method T(s: Sh): string
    return (case s of
      Circ(var r: int) => "circ" + r.ToString(),
      Mark(var v: int, var w: string) => "markboth",
      Mark(var v: int, var w) => "markv",
      Mark(var v, var w: string) => "markw",
      Mark(var v, var w) => "markany",
      Cl(var p: Pt) => "cl",
      None => "none");
  end T;

  public static method Main()
    Console.WriteLine(T(Sh.Circ(0)));
    Console.WriteLine(T(Sh.Mark(null, null)));
    Console.WriteLine(T(Sh.Mark(1, null)));
    Console.WriteLine(T(Sh.Mark(null, "a")));
    Console.WriteLine(T(Sh.Mark(2, "a")));
    Console.WriteLine(T(Sh.Cl(new Pt(1, 2))));
    Console.WriteLine(T(Sh.None));
  end Main;
end Program;
