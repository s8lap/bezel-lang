//! case: s7_5_0021
//! doc: spec
//! section: 7.5
//! title: a null-conditional call with named arguments in an async method evaluates in written order
//! rule: `?.` short-circuits. The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated — no member is read, no call is performed, and the argument and index expressions of the skipped suffixes are not evaluated either. ... A call's supplied argument expressions are evaluated in call-site textual order — left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.5, 30.1, 7.5
//! expect: run
//! output: AB|aKbc|10
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

  public static async method Work(log: StringBuilder, t: StringBuilder?): Task<int>
    await Task.Delay(1);
    var r := t?.Insert(value := TS(log, "A", "K"), index := T(log, "B", 1));
    await Task.Delay(1);
    return (if r is null then 0 else 1);
  end Work;
  public static async method Main(): Task<int>
    using var log := new StringBuilder();
    using var target := new StringBuilder();
    target.Append("abc");
    var a := await Work(log, target);
    var b := await Work(log, null);
    Console.WriteLine(log.ToString() + "|" + target.ToString() + "|" + a.ToString() + b.ToString());
    return 0;
  end Main;
end C;
