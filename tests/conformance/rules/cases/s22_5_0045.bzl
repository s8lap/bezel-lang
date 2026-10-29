//! case: s22_5_0045
//! doc: spec
//! section: 22.5
//! title: named-argument order holds when a local shadows a built-in static class name
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: Zabcdef|AB
//!   xQz|ABCD
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
    using var Path := new StringBuilder();
    Path.Append("abcdef");
    var a := Path.Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 0)).ToString();
    Console.WriteLine(a + "|" + sb.ToString());
    using var Math := new StringBuilder("xyz");
    var b := Math.Replace(newValue := TS(sb, "C", "Q"), oldValue := TS(sb, "D", "y")).ToString();
    Console.WriteLine(b + "|" + sb.ToString());
    return 0;
  end Main;
end C;
