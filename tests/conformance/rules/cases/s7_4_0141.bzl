//! case: s7_4_0141
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: Double and Float statics over narrowed locals
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). Double.IsNaN, Float.IsFinite, Double.IeeeEquals and Double.BitwiseEquals take non-nullable operands (stdlib 9.2).
//! cites: 7.1, 7.4
//! expect: run
//! output: True
//!   True
//!   False
//!   True
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := 0.0 / 0.0;
    Console.WriteLine(Double.IsNaN(d));
    var f: float? := 1.0f;
    Console.WriteLine(Float.IsFinite(f));
    Console.WriteLine(Double.IeeeEquals(d, d));
    Console.WriteLine(Double.BitwiseEquals(d, d));
    return 0;
  end Main;
end C;
