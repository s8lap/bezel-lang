//! case: s7_4_0134
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: `??` and `!` operands
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). `x ?? 0.0` has the non-nullable type double, and `x!` is non-null.
//! cites: 7.1, 7.4
//! expect: run
//! output: False
//!   7
//!   3
//!   False
//!   4
//!   2
//!   False
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method F(x: double?): int
    Console.WriteLine(Double.IsNaN(x ?? 0.0));
    Console.WriteLine(TimeSpan.FromSeconds(x ?? 7.0).TotalSeconds);
    Console.WriteLine(Math.Sqrt(x ?? 9.0));
    if x <> null then Console.WriteLine(Double.IsNaN(x!)); end;
    return 0;
  end F;
  public static method Main(): int
    F(null); F(4.0);
    return 0;
  end Main;
end C;
