//! case: s22_5_0046
//! doc: spec
//! section: 22.5
//! title: named-argument order holds inside lambda bodies
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! stdout: 7|6|abcdef|ABCD
module M;
import System;
import System.Text;
class C
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;
  public static method TS(sb: StringBuilder, s: string, v: string): string
    sb.Append(s);
    return v;
  end TS;
  public static method Mk(sb: StringBuilder, s: string, t: StringBuilder): StringBuilder
    sb.Append(s);
    return t;
  end Mk;
  public static method Pick(sb: StringBuilder, s: string, t: StringBuilder?): StringBuilder?
    sb.Append(s);
    return t;
  end Pick;
  public static method Main(): int
    using var sb := new StringBuilder();
    using var sb2 := new StringBuilder();
    sb2.Append("abcdef");
    const f := lambda(q: StringBuilder) => q.Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 0)).Length;
    var n := f(sb2);
    const g := lambda() => sb2.Remove(length := T(sb, "C", 1), startIndex := T(sb, "D", 0)).Length;
    var m := g();
    Console.WriteLine(n.ToString() + "|" + m.ToString() + "|" + sb2.ToString() + "|" + sb.ToString());
    return 0;
  end Main;
end C;
