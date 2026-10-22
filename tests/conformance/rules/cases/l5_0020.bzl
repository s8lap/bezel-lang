//! case: l5_0020
//! doc: stdlib
//! section: 5
//! title: a GroupBy materialized by ?.ToArray() lists groups in first-occurrence order with element sums
//! rule: "public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey: hashable;" ([§5]). "Groups appear in first-occurrence-of-key order; elements within a group in source order." ([§5]). "`x?.M` returns null if `x` is null; otherwise it performs the access" ([§7.5]) keeps the array non-null here.
//! cites: 7.5
//! expect: run
//! output: 1:12
//!   2:7
//!   0:3
//!   3
//!   0
module Conf;
public class P
  public static method Main(): int
    var a: Array<int> := [1, 2, 3, 4, 5, 7];
    var g := a.GroupBy(lambda(x: int) => x mod 3)?.ToArray();
    if g <> null then
      for var gr in g do
        var s := 0;
        for var v in gr do s := s + v; end;
        Console.WriteLine(gr.Key.ToString() + ":" + s.ToString());
      end;
    end;
    Console.WriteLine(a.GroupBy(lambda(x: int) => x mod 3)?.Count() ?? -1);
    var e: Array<int> := [];
    var ge := e.GroupBy(lambda(x: int) => x)?.ToArray();
    Console.WriteLine(ge?.Count ?? -1);
    return 0;
  end Main;
end P;
