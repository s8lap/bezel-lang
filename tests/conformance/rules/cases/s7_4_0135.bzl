//! case: s7_4_0135
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: nested stdlib calls over a narrowed local
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). Math.Abs, Math.Sqrt and Math.Max accept the narrowed local.
//! cites: 7.1, 7.4
//! expect: run
//! output: 2
//!   True
//!   False
//!   4
//!   4
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := -4.0;
    Console.WriteLine(Math.Sqrt(Math.Abs(d)));
    Console.WriteLine(Double.IsNaN(Math.Sqrt(d)));
    Console.WriteLine(Double.IsNaN(Math.Abs(d)));
    Console.WriteLine(TimeSpan.FromSeconds(Math.Abs(d)).TotalSeconds);
    Console.WriteLine(Math.Max(Math.Abs(d), d));
    return 0;
  end Main;
end C;
