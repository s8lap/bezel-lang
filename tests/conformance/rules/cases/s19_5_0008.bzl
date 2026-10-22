//! case: s19_5_0008
//! doc: spec
//! section: 19.5
//! title: a typed binding at a nested record slot and at a nullable record slot matches only
//!   present values
//! rule: "A typed binding pattern `var n: T` also matches only non-null values — the type
//!   annotation makes it a type-constrained binding, not an unconditional binding. `null` is
//!   matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind
//!   any value including null), or by `_`." (§19.5, v0.340) The rule applies at every slot
//!   of a nested destructure: `Pair(var a: Object, var n)` fails when the nested `Pair`'s
//!   `Object?` slot is `null`, and `var s: string` fails when `Outer`'s `string?` slot is
//!   `null`.
//! cites: 19.3
//! expect: run
//! output: False
//!   True
//!   True
//!   False
//!   True
module Conf;

public record Pair(A: Object?, B: int);
public record Outer(P: Pair?, Q: string?);

public class Program
  public static method Main()
    var p: Pair := new Pair(null, 3);
    var q: Pair := new Pair("s", 3);
    Console.WriteLine((case p of Pair(var a: Object, var n) => true, _ => false));
    Console.WriteLine((case q of Pair(var a: Object, var n) => true, _ => false));
    Console.WriteLine((case p of Pair(var a, var n) => true, _ => false));
    var o1: Outer := new Outer(new Pair("s", 1), null);
    var o2: Outer := new Outer(new Pair("s", 1), "x");
    Console.WriteLine((case o1 of Outer(Pair(var a: Object, var n), var s: string) => true, _ => false));
    Console.WriteLine((case o2 of Outer(Pair(var a: Object, var n), var s: string) => true, _ => false));
  end Main;
end Program;
