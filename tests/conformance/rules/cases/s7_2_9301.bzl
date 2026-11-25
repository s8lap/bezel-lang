//! case: s7_2_9301
//! doc: spec
//! section: 7.2
//! title: control: a non-generic nullable record and a nullable choice hold a value or null
//! rule: A value type wrapped in `?` is an optional: it either holds a value or `null`.
//! cites: 7.2, 35.4
//! expect: run
//! output: 1
//!   q
//! notes: Harvested from the #9049 review (ctl/c2), the non-generic control for s14_1_9301..5. Trace: `a` holds
//!   P(1, "y") and `b` is null; the array holds both; the loop adds X (1) for the non-null element and skips
//!   the null one: n = 1. `c` holds C.B("q"); the case selects B and yields its payload "q".
module Conf;
public record P(X: int, Y: string);
public choice C A(n: int), B(s: string); end C;
public class Program
  public static method Main(): int
    var a: P? := new P(1, "y");
    var b: P? := null;
    var c: C? := C.B("q");
    using var arr := new Array<P?>();
    arr.Add(a); arr.Add(b);
    var n := 0;
    for var x in arr do if x is not null then n := n + x!.X; end if; end for;
    Console.WriteLine(n);
    Console.WriteLine((case c of A(var i) => "a", B(var s) => s, _ => "null"));
    return 0;
  end Main;
end Program;
