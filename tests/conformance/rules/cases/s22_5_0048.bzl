//! case: s22_5_0048
//! doc: spec
//! section: 22.5
//! title: named-argument order holds after a field-chain receiver
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as written, for positional and named arguments alike; naming an argument changes which parameter it binds to, never when it runs (spec 22.5 Evaluation order, v0.350, example M(b := Next(), a := Next())). The receiver is an operand written before the arguments and is evaluated first.
//! cites: 22.5
//! expect: run
//! output: Zabc|RAB
//!   YZabc|AB
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
end C;
class H
  public owned Inner: StringBuilder;
  public constructor()
    Inner := new StringBuilder("abc");
  end constructor;
  public method Self(sb: StringBuilder, s: string): H
    sb.Append(s);
    return this;
  end Self;
end H;
class D
  public static method Main(): int
    using var sb := new StringBuilder();
    using var h := new H();
    var a := h.Self(sb, "R").Inner.Insert(value := C.TS(sb, "A", "Z"), index := C.T(sb, "B", 0)).ToString();
    Console.WriteLine(a + "|" + sb.ToString());
    sb.Clear();
    var b := h.Inner.Insert(value := C.TS(sb, "A", "Y"), index := C.T(sb, "B", 0)).ToString();
    Console.WriteLine(b + "|" + sb.ToString());
    return 0;
  end Main;
end D;
