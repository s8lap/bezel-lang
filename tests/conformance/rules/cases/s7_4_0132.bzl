//! case: s7_4_0132
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: a readonly field narrowed by a null test
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). The readonly field `_n` narrows under `_n <> null`.
//! cites: 7.1, 7.4
//! expect: run
//! output: False
//!   2.5
//!   null
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  private readonly _n: double?;
  public constructor(n: double?) _n := n; end;
  public method Run(): int
    if _n <> null then
      Console.WriteLine(Double.IsNaN(_n));
      Console.WriteLine(TimeSpan.FromSeconds(_n).TotalSeconds);
    else
      Console.WriteLine("null");
    end;
    return 0;
  end Run;
  public static method Main(): int
    var a := new C(2.5);
    var b := new C(null);
    a.Run(); b.Run();
    return 0;
  end Main;
end C;
