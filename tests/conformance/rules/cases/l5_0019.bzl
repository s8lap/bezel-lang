//! case: l5_0019
//! doc: stdlib
//! section: 5
//! title: an IGrouping parameter answers Count() and Key in a per-group call
//! rule: "public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey:
//!   hashable;" (stdlib §5) groups the array. "public interface IGrouping<TKey, T>: IEnumerable<T>" and
//!   "readonly property Key: TKey;" (stdlib §5) type the parameter and its key, and "public method
//!   Count(): int;" (stdlib §5) is the count. The groups of [1, 2, 2, 3] have keys 1, 2, 3 and counts 1,
//!   2, 1, so `Sz` returns 2, 4 and 4.
//! cites: 5
//! expect: run
//! output: 2
//!   4
//!   4
//! issue: 6714
module Conf;
class C
  public static method Sz(g: IGrouping<int, int>): int
    return g.Count() + g.Key;
  end Sz;
  public static method Main(): int
    var a: Array<int> := [1, 2, 2, 3];
    for var g in a.GroupBy(lambda(x: int) => x) do
      Console.WriteLine(Sz(g));
    end;
    return 0;
  end Main;
end C;
