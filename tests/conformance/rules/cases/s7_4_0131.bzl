//! case: s7_4_0131
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: a loop that reassigns a non-null value keeps the
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). A loop body that only reassigns non-null values keeps the narrowing at the loop head.
//! cites: 7.1, 7.4
//! expect: run
//! output: False
//!   False
//!   False
//!   2.5
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := 1.5;
    var i := 0;
    while i < 3 do
      Console.WriteLine(Double.IsNaN(d));
      d := 2.5;
      i := i + 1;
    end;
    Console.WriteLine(TimeSpan.FromSeconds(d).TotalSeconds);
    return 0;
  end Main;
end C;
