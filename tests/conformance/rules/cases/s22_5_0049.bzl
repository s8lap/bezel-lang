//! case: s22_5_0049
//! doc: spec
//! section: 22.5
//! title: named-argument order holds for built-in String methods (Substring, ...
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: ell|RBA
//!   hello|RBA
//!   heLLo worLd|RBA
//!   hello W|RBA
//!   True|RBA
//!   |A
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
  public static method TC(sb: StringBuilder, s: string, v: char): char
    sb.Append(s);
    return v;
  end TC;
  public static method TB(sb: StringBuilder, s: string, v: bool): bool
    sb.Append(s);
    return v;
  end TB;
  public static method R(sb: StringBuilder, s: string): string
    sb.Append(s);
    return "hello world";
  end R;
  public static method Main(): int
    using var sb := new StringBuilder();
    Console.WriteLine(R(sb, "R").Substring(length := T(sb, "B", 3), startIndex := T(sb, "A", 1)) + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(R(sb, "R").Remove(length := T(sb, "B", 6), startIndex := T(sb, "A", 5)) + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(R(sb, "R").Replace(newChar := TC(sb, "B", 'L'), oldChar := TC(sb, "A", 'l')) + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(R(sb, "R").Replace(newValue := TS(sb, "B", "W"), oldValue := TS(sb, "A", "world")) + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(R(sb, "R").Equals(ignoreCase := TB(sb, "B", true), other := TS(sb, "A", "HELLO WORLD")).ToString() + "|" + sb.ToString()); sb.Clear();
    Console.WriteLine(string.Join(items := new Array<string>(), separator := TS(sb, "A", ",")) + "|" + sb.ToString()); sb.Clear();
    return 0;
  end Main;
end C;
