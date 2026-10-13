//! case: s28_3_0154
//! doc: spec
//! section: 28.3
//! title: transferring the source of a live lazy-query local is rejected
//! rule: A live local holding an iterator or enumerator frame or a lazy query ...
//!   counts as a live local borrow of every source the frame carries ... `sink.Take(move xs);` while `it` or `q` has a later use is rejected
//! cites: 28.3
//! expect: reject
//! diagnostic: live
//! issue: 5698
module Conf;


public class Program
  public static method Take(owned a: Array<int>): int => a.Count;

  public static method Run(): int
    var xs: Array<int> := [1, 2, 3];
    var q := xs.Where(lambda(v) => v > 1);
    var n := Take(move xs);
    for var x in q do
      n := n + x;
    end;
    return n;
  end Run;
end Program;
