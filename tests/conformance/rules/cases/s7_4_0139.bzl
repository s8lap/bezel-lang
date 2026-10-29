//! case: s7_4_0139
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: a reassignment inside the loop before the use
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). `d := i * 1.5` has a non-nullable type, so d is narrowed at the use.
//! cites: 7.1, 7.4
//! expect: run
//! output: False
//!   0
//!   False
//!   1.5
//!   False
//!   3
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := null;
    var i := 0;
    while i < 3 do
      d := i * 1.5;
      Console.WriteLine(Double.IsNaN(d));
      Console.WriteLine(TimeSpan.FromSeconds(d).TotalSeconds);
      i := i + 1;
    end;
    return 0;
  end Main;
end C;
