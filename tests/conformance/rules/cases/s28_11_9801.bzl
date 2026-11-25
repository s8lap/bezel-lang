//! case: s28_11_9801
//! doc: spec
//! section: 28.11
//! title: A choice stored into a Dictionary from a field survives the Dictionary's Clear(), 200x
//! rule: `Clear()` | removes all | removes all | removes all
//! cites: 28.11, 28.9
//! expect: run
//! output: 1000
//! notes: Pins #9197/#9254 (picks 0c6b7d2da, 201f50859): a Dictionary<string, Shape> slot copies a choice
//!   whose payload is a string, taking its own reference on the string; removing the copy (Clear,
//!   Remove, a declined TryAdd) must leave the source's string readable. The leak half needs the
//!   sanitizer leg the corpus runner lacks; this case pins the observable half over a 200x loop.
//!   Source in a field: Clear removes the dictionary's copy only; the field's string f1000.. has 5
//!   characters each time: 1000.
module Conf;

public choice Shape Circle(r: int), Named(n: string); end Shape;

public class FH
  private _s: Shape := Shape.Circle(1);
  private owned _d: Dictionary<string, Shape> := new Dictionary<string, Shape>();
  public method SetS(i: int) => _s := Shape.Named("f" + (i + 1000).ToString());
  public method Store() => _d["a"] := _s;
  public method ClearD() => _d.Clear();
  public method ReadS(): int => (case _s of Named(n) => n.Length, Circle(r) => 0);
end FH;

public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      using var h := new FH();
      h.SetS(i);
      h.Store();
      h.ClearD();
      total := total + h.ReadS();
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
