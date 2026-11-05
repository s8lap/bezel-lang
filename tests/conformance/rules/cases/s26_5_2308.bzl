//! case: s26_5_2308
//! doc: spec
//! section: 26.5
//! title: the ?. spelling of a predicate Count on a SortedSet is refused
//! rule: the call is a compile error citing the instance member(s); it does
//!   not fall back to an extension
//! cites: 5, 26.5, 7.5
//! expect: reject
//! diagnostic: instance member
//! notes: Green on base and tip (#7670 refused the ?. spelling before #7812
//!   refused the plain one).
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    var x: SortedSet<int>? := s;
    var r := x?.Count(lambda(v: int) => v > 1);
    Console.WriteLine(r);
  end Main;
end Program;
