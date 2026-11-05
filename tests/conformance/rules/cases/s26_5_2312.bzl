//! case: s26_5_2312
//! doc: spec
//! section: 26.5
//! title: reading the Count property on sorted collections still resolves
//!   to the instance member
//! rule: If `x`'s declared type (or any base type) *declares* any instance
//!   member named `Foo`
//! cites: 5, 26.5
//! expect: run
//! output: 3
//!   2
//! notes: Accepting control (green on base and tip): the property read is
//!   the committed instance member.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    var sd: SortedDictionary<int, int> := new SortedDictionary<int, int>();
    _ := sd.TryAdd(1, 10);
    _ := sd.TryAdd(2, 20);
    Console.WriteLine(s.Count);
    Console.WriteLine(sd.Count);
  end Main;
end Program;
