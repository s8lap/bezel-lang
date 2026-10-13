//! case: s28_3_0157
//! doc: spec
//! section: 28.3
//! title: a bound iterator frame explicitly disposed before the transfer frees its source
//! rule: The frame's own disposal is such a use — an explicit `Dispose()`, a
//!   `using var`'s scope end, or K3.14's auto-drop ... so a bound frame keeps its
//!   sources frozen until it is disposed or moved ... or dispose it
//!   (`it.Dispose();`) before the transfer
//! cites: 28.3, 29.7
//! expect: run
//! stdout: 9
//! issue: 5698
module Conf;

public class Program
  public static method Each(xs: Array<int>): IEnumerable<int>
    for var x in xs do
      yield return x;
    end;
  end Each;

  public static method Take(owned a: Array<int>): int => a.Count;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var it := Each(xs);
    var n := 0;
    for var x in it do
      n := n + x;
    end;
    it.Dispose();
    n := n + Take(move xs);
    Console.WriteLine(n.ToString());
  end Main;
end Program;
