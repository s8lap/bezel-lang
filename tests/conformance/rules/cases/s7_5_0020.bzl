//! case: s7_5_0020
//! doc: spec
//! section: 7.5
//! title: a null-conditional call with named arguments in a lambda and a generic method evaluates in order
//! rule: `?.` short-circuits. The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated — no member is read, no call is performed, and the argument and index expressions of the skipped suffixes are not evaluated either. ... A call's supplied argument expressions are evaluated in call-site textual order — left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.5, 14.1, 7.5
//! expect: run
//! output: AB|abLc|null
//!   ABAB|ggabLc|110
module Conf;
import System.Text;
class Node
  public sb: StringBuilder? := null;
  public next: Node? := null;
  public constructor(sb: StringBuilder?, next: Node?) this.sb := sb; this.next := next; end constructor;
end Node;
class C
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s); return v;
  end T;
  public static method TS(sb: StringBuilder, s: string, v: string): string
    sb.Append(s); return v;
  end TS;

  public static method G<X>(sb: StringBuilder?, log: StringBuilder, x: X): int
    var r := sb?.Insert(value := TS(log, "A", "g"), index := T(log, "B", 0));
    return (if r is null then 0 else 1);
  end G;
  public static method Main(): int
    using var log := new StringBuilder();
    using var target := new StringBuilder();
    target.Append("abc");
    var f := lambda(s: StringBuilder?): StringBuilder? => s?.Insert(value := TS(log, "A", "L"), index := T(log, "B", 2));
    var x := f(target);
    var y := f(null);
    Console.WriteLine(log.ToString() + "|" + target.ToString() + "|" + (if y is null then "null" else "nn")); log.Clear();
    var g1 := G<int>(target, log, 1);
    var g2 := G<string>(target, log, "s");
    var g3 := G<int>(null, log, 1);
    Console.WriteLine(log.ToString() + "|" + target.ToString() + "|" + g1.ToString() + g2.ToString() + g3.ToString()); log.Clear();
    return 0;
  end Main;
end C;
