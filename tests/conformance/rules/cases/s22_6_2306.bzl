//! case: s22_6_2306
//! doc: spec
//! section: 22.6
//! title: a lambda returning a LinkedList or Set parameter at an
//!   IEnumerable slot iterates the elements
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3
//! expect: run
//! output: 14
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(l: LinkedList<int>, f: (LinkedList<int>) -> IEnumerable<int>): int
    var n := 0;
    for var v in f(l) do n := n + v; end;
    return n;
  end Run;
  public static method RunS(l: Set<int>, f: (Set<int>) -> IEnumerable<int>): int
    var n := 0;
    for var v in f(l) do n := n + v; end;
    return n;
  end RunS;
end H;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    _ := l.AddLast(1);
    _ := l.AddLast(2);
    using var s := new Set<int>();
    s.Add(5);
    s.Add(6);
    var t := H.Run(l, lambda(x) => x) + H.RunS(s, lambda(x) => do return x; end);
    Console.WriteLine(t);
  end Main;
end Program;
