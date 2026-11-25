//! case: s7_5_9002
//! doc: spec
//! section: 7.5
//! title: the `?.` spelling of SortedSet's declared First, Last and Count properties is admitted
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 26.5
//! expect: run
//! output: 1
//!   3
//!   2
//! notes: Harvested from the #8783 producer's corpus-candidates (l4_18_9001, l4_18_9002 merged; the first
//!   candidate's stated output "1 3" was two lines on one). Stdlib 4.18 declares `First`, `Last` and
//!   `Count` on `SortedSet<T>`. Trace: the set holds {1, 3}: First = 1, Last = 3, Count = 2.
module Conf;
public class Program
  public static method Main()
    using var s := new SortedSet<int>();
    s.Add(3);
    s.Add(1);
    var q: SortedSet<int>? := s;
    Console.WriteLine(q?.First ?? -1);
    Console.WriteLine(q?.Last ?? -1);
    Console.WriteLine(q?.Count ?? -1);
  end Main;
end Program;
