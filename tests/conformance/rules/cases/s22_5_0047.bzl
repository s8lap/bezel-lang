//! case: s22_5_0047
//! doc: spec
//! section: 22.5
//! title: named-argument order holds in a generator body across iterations
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! stdout: 12|RABRABRAB|ZZZab
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
  public static method Gen(sb: StringBuilder, sb2: StringBuilder): IEnumerable<int>
    for var i in 0..<3 do
      yield return Mk(sb, "R", sb2).Insert(value := TS(sb, "A", "Z"), index := T(sb, "B", 0)).Length;
    end for;
  end Gen;
  public static method Main(): int
    using var sb := new StringBuilder();
    using var sb2 := new StringBuilder();
    sb2.Append("ab");
    var tot := 0;
    for var x in Gen(sb, sb2) do
      tot += x;
    end for;
    Console.WriteLine(tot.ToString() + "|" + sb.ToString() + "|" + sb2.ToString());
    return 0;
  end Main;
end C;
