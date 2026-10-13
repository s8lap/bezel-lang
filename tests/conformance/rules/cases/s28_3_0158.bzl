//! case: s28_3_0158
//! doc: spec
//! section: 28.3
//! title: a using var iterator frame keeps its source frozen to its scope end
//! rule: The frame's own disposal is such a use — an explicit `Dispose()`, a
//!   `using var`'s scope end, or K3.14's auto-drop — because a partially-consumed
//!   frame's disposal runs its pending cleanup, `finally` blocks included, which
//!   may read the carried sources
//! cites: 28.3, 29.7
//! expect: reject
//! diagnostic: live
//! issue: 5698
module Conf;

public class Program
  public static method Each(xs: Array<int>): IEnumerable<int>
    for var x in xs do
      yield return x;
    end;
  end Each;

  public static method Take(owned a: Array<int>): int => a.Count;

  public static method Run(): int
    var xs: Array<int> := [1, 2, 3];
    using var it := Each(xs);
    return Take(move xs);
  end Run;
end Program;
