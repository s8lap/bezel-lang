//! case: s7_4_0142
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: StringBuilder.Append over narrowed decimal, floa
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). Append has per-type overloads for the numeric primitives (stdlib 3.1); a decimal renders without trailing zeros (stdlib 9.3).
//! cites: 7.1, 7.4
//! expect: run
//! output: 2.5|2.5
//!   1.5|1.5|5000000000|5000000000
module M; import System; import System.Text;
class C
  public static method Main(): int
    var m: decimal? := 2.50m;
    var m2: decimal := 2.50m;
    var sb := new StringBuilder();
    sb.Append(m); sb.Append("|"); sb.Append(m2);
    Console.WriteLine(sb.ToString());
    var f: float? := 1.5f; var f2: float := 1.5f; var l: long? := 5000000000; var l2: long := 5000000000;
    var sb2 := new StringBuilder();
    sb2.Append(f); sb2.Append("|"); sb2.Append(f2); sb2.Append("|"); sb2.Append(l); sb2.Append("|"); sb2.Append(l2);
    Console.WriteLine(sb2.ToString());
    return 0;
  end Main;
end C;
