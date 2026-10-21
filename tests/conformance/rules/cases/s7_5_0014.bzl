//! case: s7_5_0014
//! doc: spec
//! section: 7.5
//! title: ?. on a nullable IGrouping with ?? defaults yields the group key and count
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access" ([§7.5]), and "`a ?? b`
//!   returns `a` if non-null, else `b`" ([§7.7]). The groups of [1, 2, 2] have keys 1 and 2 with counts 1
//!   and 2, and each iteration prints the key and then the count: 1, 1, 2, 2.
//! cites: 7.5, 7.7, 5
//! expect: run
//! output: 1
//!   1
//!   2
//!   2
//! issue: 6714
module Conf;
class C
  public static method Main(): int
    var a: Array<int> := [1, 2, 2];
    for var g in a.GroupBy(lambda(x: int) => x) do
      var h: IGrouping<int, int>? := g;
      var k := h?.Key;
      var c := h?.Count();
      Console.WriteLine(k ?? -1);
      Console.WriteLine(c ?? -1);
    end;
    return 0;
  end Main;
end C;
