//! case: s7_5_0019
//! doc: spec
//! section: 7.5
//! title: named arguments of a null-conditional call evaluate in written order, skipped on null
//! rule: `?.` short-circuits. The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated — no member is read, no call is performed, and the argument and index expressions of the skipped suffixes are not evaluated either. ... A call's supplied argument expressions are evaluated in call-site textual order — left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.5, 7.5
//! expect: run
//! output: AB|aZbc
//!   |aZbc
//!   |aZbc
//!   AB|WaZbc
//!   ABCD|WaZbc
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

  public static method Main(): int
    using var log := new StringBuilder();
    using var target := new StringBuilder();
    target.Append("abc");
    // nested chain
    var n2 := new Node(target, null);
    var n1 := new Node(null, n2);
    var r := n1.next?.sb?.Insert(value := TS(log, "A", "Z"), index := T(log, "B", 1));
    Console.WriteLine(log.ToString() + "|" + target.ToString()); log.Clear();
    var r2 := n1.sb?.Insert(value := TS(log, "A", "Q"), index := T(log, "B", 1));
    Console.WriteLine(log.ToString() + "|" + target.ToString()); log.Clear();
    // ?? around
    var r3 := (n1.sb?.Insert(value := TS(log, "A", "Q"), index := T(log, "B", 1))) ?? target;
    Console.WriteLine(log.ToString() + "|" + r3.ToString()); log.Clear();
    var r4 := (n2.sb?.Insert(value := TS(log, "A", "W"), index := T(log, "B", 0))) ?? log;
    Console.WriteLine(log.ToString() + "|" + r4.ToString()); log.Clear();
    // chained after
    var r5 := n2.sb?.Insert(value := TS(log, "A", "Y"), index := T(log, "B", 0))?.Remove(length := T(log, "C", 1), startIndex := T(log, "D", 0));
    Console.WriteLine(log.ToString() + "|" + target.ToString()); log.Clear();
    return 0;
  end Main;
end C;
