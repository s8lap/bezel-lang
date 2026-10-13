//! case: s28_3_0156
//! doc: spec
//! section: 28.3
//! title: transferring the source inside a for body over an iterator frame of it is rejected
//! rule: A live local holding an iterator or enumerator frame or a lazy query —
//!   `var it := Each(xs.AsSpan());`, `var q := xs.Where(f);`, the `for`
//!   desugaring's hidden `__source` (§29.2) — counts as a live local borrow of
//!   every source the frame carries
//! cites: 28.3, 29.2
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
    for var x in Each(xs) do
      return Take(move xs);
    end;
    return 0;
  end Run;
end Program;
