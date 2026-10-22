//! case: s7_5_0016
//! doc: spec
//! section: 7.5
//! title: a null-conditional GroupBy then ToArray keeps the chain non-null and its Count is the group count
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access" ([§7.5]). "public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey: hashable;" (stdlib §5) puts 1 and 3 in one group and 2 and 4 in another for `x mod 2`, so the array has 2 groups and the count is 2.
//! cites: 5
//! expect: run
//! output: 2
module Conf;
public class P
  public static method Main(): int
    var a: Array<int> := [1, 2, 3, 4];
    var g := a.GroupBy(lambda(x: int) => x mod 2)?.ToArray();
    if g = null then Console.WriteLine("NULL"); else Console.WriteLine(g.Count); end;
    return 0;
  end Main;
end P;
