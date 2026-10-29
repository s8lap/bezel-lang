//! case: s22_5_0043
//! doc: spec
//! section: 22.5
//! title: named arguments around an await are evaluated in textual order
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: cdef|CD
//!   cef|CD
//!   ef|RAB
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
  public static async method SlowS(sb: StringBuilder, s: string, v: string): Task<string>
    await Task.Delay(5);
    sb.Append(s);
    return v;
  end SlowS;
  public static async method SlowI(sb: StringBuilder, s: string, v: int): Task<int>
    await Task.Delay(5);
    sb.Append(s);
    return v;
  end SlowI;
  public static async method Main(): Task<int>
    using var sb := new StringBuilder();
    using var sb2 := new StringBuilder();
    sb2.Append("abcdef");
    var a := sb2.Remove(length := await SlowI(sb, "C", 2), startIndex := T(sb, "D", 0)).ToString();
    Console.WriteLine(a + "|" + sb.ToString());
    sb.Clear();
    var b := sb2.Remove(await SlowI(sb, "C", 1), T(sb, "D", 1)).ToString();
    Console.WriteLine(b + "|" + sb.ToString());
    sb.Clear();
    var c := Mk(sb, "R", sb2).Remove(length := T(sb, "A", 1), startIndex := await SlowI(sb, "B", 0)).ToString();
    Console.WriteLine(c + "|" + sb.ToString());
    return 0;
  end Main;
end C;
