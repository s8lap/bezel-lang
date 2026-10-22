//! case: l4_18_0006
//! doc: stdlib
//! section: 4.18
//! title: a SortedSet of a class element iterates in CompareTo order after the set is reassigned
//! rule: "A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order."
//! cites: 4.18
//! expect: run
//! output: 1
//!   3
module Conf;

public class Cmp: IComparable<Cmp>
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public method CompareTo(other: Cmp): int
    if N < other.N then
      return -1;
    end;
    if N > other.N then
      return 1;
    end;
    return 0;
  end CompareTo;
end Cmp;

public class Program
  public static method Main()
    var a := new Cmp(3);
    var b := new Cmp(1);
    var s := new SortedSet<Cmp>();
    s.Add(a);
    s := new SortedSet<Cmp>();
    s.Add(a);
    s.Add(b);
    for var x in s do
      Console.WriteLine(x.N);
    end;
  end Main;
end Program;
