//! case: s7_4_0133
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: a narrowing survives an await when the local is 
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). No lambda captures d, so an await does not invalidate the narrowing.
//! cites: 7.1, 7.4
//! expect: run
//! output: False
//!   1.5
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static async method F(): Task<int>
    var d: double? := 1.5;
    await Task.Delay(1);
    Console.WriteLine(Double.IsNaN(d));
    Console.WriteLine(TimeSpan.FromSeconds(d).TotalSeconds);
    return 0;
  end F;
  public static method Main(): int
    return F().Result;
  end Main;
end C;
