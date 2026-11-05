//! case: s26_5_2303
//! doc: spec
//! section: 26.5
//! title: a predicate Count on a SortedDictionary is refused
//! rule: the call is a compile error citing the instance member(s); it does
//!   not fall back to an extension
//! cites: 5, 26.5
//! expect: reject
//! diagnostic: instance member
//! issue: 7812
//! notes: Red on base: the pre-fix compiler accepted the plain `.` spelling
//!   of the predicate Count on this receiver (it fell through to the LINQ
//!   extension), the staging tip refuses it.
module Conf;

public class Program
  public static method Main()
    var sd: SortedDictionary<int, int> := new SortedDictionary<int, int>();
    _ := sd.TryAdd(1, 10);
    _ := sd.TryAdd(2, 20);
    Console.WriteLine(sd.Count(lambda(e: DictionaryEntry<int, int>) => e.Value > 10));
  end Main;
end Program;
