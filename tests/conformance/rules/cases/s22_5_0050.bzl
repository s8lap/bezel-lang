//! case: s22_5_0050
//! doc: spec
//! section: 22.5
//! title: named-argument order holds for built-in static calls (Math.Max, Math.Clamp, Convert.ToString)
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: 9|BA
//!   5|BAC
//!   101|BA
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
  public static method TD(sb: StringBuilder, s: string, v: double): double
    sb.Append(s);
    return v;
  end TD;
  public static method Main(): int
    using var sb := new StringBuilder();
    Console.WriteLine(Math.Max(b := T(sb, "B", 3), a := T(sb, "A", 9)).ToString() + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(Math.Clamp(high := T(sb, "B", 5), value := T(sb, "A", 9), low := T(sb, "C", 1)).ToString() + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(Convert.ToString(toBase := T(sb, "B", 2), value := T(sb, "A", 5)) + "|" + sb.ToString()); sb.Clear();
    return 0;
  end Main;
end C;
