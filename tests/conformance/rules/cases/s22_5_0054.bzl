//! case: s22_5_0054
//! doc: spec
//! section: 22.5
//! title: named-argument order holds on fresh temporary receivers in a loop
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! stdout: 400|ABBA
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
  public static method Boom(sb: StringBuilder): int
    sb.Append("X");
    throw new InvalidOperationException("boom");
  end Boom;
  public static method Main(): int
    using var sb := new StringBuilder();
    var total := 0;
    for var i in 0..<100 do
      total += new StringBuilder("abc").Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 1)).Length;
      new StringBuilder("abcdef").Remove(length := T(sb, "B", 2), startIndex := T(sb, "A", 1));
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.ToString().Substring(0, 4));
    return 0;
  end Main;
end C;
