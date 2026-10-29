//! case: s22_5_0052
//! doc: spec
//! section: 22.5
//! title: named-argument order holds for Encoding.GetBytes with a span argument
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: 3|A
//!   2|AB
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
    using var buf := new OwningBuffer<byte>(16);
    var n := Encoding.UTF8.GetBytes(destination := buf.AsSpan(), s := TS(sb, "A", "hey"));
    Console.WriteLine(n.ToString() + "|" + sb.ToString());
    var m := Encoding.ASCII.GetBytes(destination := buf.AsSpan(), s := TS(sb, "B", "hi"));
    Console.WriteLine(m.ToString() + "|" + sb.ToString());
    return 0;
  end Main;
end C;
