//! case: s7_4_0075
//! doc: spec
//! section: 7.4
//! title: a narrowed IGrouping local answers ?. Count and Key and fills a dictionary
//! rule: "from a declaration whose initializer is such an `expr`" ([§7.4]): `var h: IGrouping<int, int>? :=
//!   g;` narrows `h`, because `g` is non-nullable. "`x?.M` returns null if `x` is null; otherwise it
//!   performs the access" ([§7.5]) gives the count and the key, which print 1, 1, then 2, 2, then 1, 3.
//!   "public method Add(key: K, value: V);" and "public pure readonly property Count: int;" (stdlib §4.8)
//!   make the dictionary count 3.
//! cites: 7.4, 7.5, 4.8
//! expect: run
//! output: 1
//!   1
//!   2
//!   2
//!   1
//!   3
//!   3
//! issue: 6714
module Conf;
class C
  public static method Main(): int
    var a: Array<int> := [1, 2, 2, 3];
    var d := new Dictionary<int, int>();
    for var g in a.GroupBy(lambda(x: int) => x) do
      var h: IGrouping<int, int>? := g;
      Console.WriteLine($"{h?.Count()}");
      Console.WriteLine($"{h?.Key}");
      d.Add(g.Key, g.Count());
    end;
    Console.WriteLine(d.Count);
    return 0;
  end Main;
end C;
