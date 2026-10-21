//! case: s7_5_0015
//! doc: spec
//! section: 7.5
//! title: a null-conditional GroupBy then Count inside WriteLine prints its count, or -3 for null
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access" ([§7.5]), and "`a ?? b`
//!   returns `a` if non-null, else `b`" ([§7.7]). "public method GroupBy<TKey>(key: (T) -> TKey):
//!   IEnumerable<IGrouping<TKey, T>> where TKey: hashable;" (stdlib §5) gives two groups for [1, 2], so
//!   the first line is 2. The null array makes the chain null, and `?? -3` gives -3.
//! cites: 7.5, 7.7, 5
//! expect: run
//! output: 2
//!   -3
//! issue: 6664
module Conf;
class C
  public static method Main(): int
    var a: Array<int>? := [1, 2];
    Console.WriteLine(a?.GroupBy(lambda(x: int) => x)?.Count() ?? -2);
    var b: Array<int>? := null;
    Console.WriteLine(b?.GroupBy(lambda(x: int) => x)?.Count() ?? -3);
    return 0;
  end Main;
end C;
