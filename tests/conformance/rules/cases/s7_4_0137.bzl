//! case: s7_4_0137
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: an early return on `x = null` narrows the surviv
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). After `if x = null then return; end;` the surviving path is narrowed.
//! cites: 7.1, 7.4
//! expect: run
//! output: null
//!   False
//!   False
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method F(x: double?): int
    if x = null then
      Console.WriteLine("null");
      return 1;
    end;
    Console.WriteLine(Double.IsNaN(x));
    Console.WriteLine(Double.IsInfinity(x));
    return 0;
  end F;
  public static method Main(): int
    F(null); F(3.0);
    return 0;
  end Main;
end C;
