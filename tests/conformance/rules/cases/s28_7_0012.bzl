//! case: s28_7_0012
//! doc: spec
//! section: 28.7
//! title: reassigning an owned SortedSet<int> field leaves the field holding only the new elements
//! rule: "Assignment to an owned field or owned local disposes the displaced value."
//! cites: 4.18, 28.5
//! expect: run
//! output: 2
//!   True
//!   False
module Conf;

public class Bag
  private owned _s: SortedSet<int>;

  public constructor()
    _s := new SortedSet<int>();
  end constructor;

  public method Reset()
    _s := new SortedSet<int>();
  end Reset;

  public method Add(x: int)
    _s.Add(x);
  end Add;

  public method Count(): int => _s.Count;

  public method Has(x: int): bool => _s.Contains(x);
end Bag;

public class Program
  public static method Main()
    using var b := new Bag();
    b.Add(9);
    b.Add(4);
    b.Reset();
    b.Add(8);
    b.Add(2);
    Console.WriteLine(b.Count());
    Console.WriteLine(b.Has(8));
    Console.WriteLine(b.Has(9));
  end Main;
end Program;
