//! case: s7_5_9001
//! doc: spec
//! section: 7.5
//! title: the `?.` spelling of SortedDictionary's declared FirstKey property is admitted and yields the key
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 26.5
//! expect: run
//! output: 1
//! notes: Harvested from the #8783 producer's corpus-candidates (l4_17_9001). Stdlib 4.17 declares `public
//!   pure readonly property FirstKey: K;` on `SortedDictionary<K, V>`. Trace: after Add(1, 10) the least
//!   key is 1, and the nullable receiver holds the dictionary, so `q?.FirstKey` is 1.
module Conf;
public class Program
  public static method Main()
    using var d := new SortedDictionary<int, int>();
    d.Add(1, 10);
    var q: SortedDictionary<int, int>? := d;
    Console.WriteLine(q?.FirstKey ?? -1);
  end Main;
end Program;
