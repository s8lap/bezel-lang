//! case: s22_5_0042
//! doc: spec
//! section: 22.5
//! title: named arguments after a null-forgiving receiver: textual order, and a null receiver faults ...
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5, 17.8
//! expect: run
//! output: RAB|4|aZbc
//!   NRE
//!   R
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
    sb2.Append("abc");
    var n1 := Pick(sb, "R", sb2)!.Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 1)).Length;
    Console.WriteLine(sb.ToString() + "|" + n1.ToString() + "|" + sb2.ToString());
    sb.Clear();
    try
      var n2 := Pick(sb, "R", null)!.Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 1)).Length;
      Console.WriteLine(n2.ToString());
    catch
      NullReferenceException => Console.WriteLine("NRE"),
    end try;
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end C;
