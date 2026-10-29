//! case: s7_4_0136
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: named arguments over a narrowed local
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). Named arguments bind the same non-nullable parameters.
//! cites: 7.1, 7.4
//! expect: run
//! output: 1.4142135623730951
//!   4
//!   9
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := 2.0;
    Console.WriteLine(Math.Sqrt(x := d));
    Console.WriteLine(Math.Pow(y := d, x := d));
    Console.WriteLine(Math.Pow(x := 3.0, y := d));
    return 0;
  end Main;
end C;
