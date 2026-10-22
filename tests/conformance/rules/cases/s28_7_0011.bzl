//! case: s28_7_0011
//! doc: spec
//! section: 28.7
//! title: reassigning an owned Set<int> field leaves the field holding only the new elements
//! rule: "Assignment to an owned field or owned local disposes the displaced value."
//! cites: 4.10, 28.5
//! expect: run
//! output: 1
module Conf;

public class Bag
  private owned _s: Set<int>;

  public constructor()
    _s := new Set<int>();
  end constructor;

  public method Reset()
    _s := new Set<int>();
  end Reset;

  public method Add(x: int)
    _s.Add(x);
  end Add;

  public method Count(): int => _s.Count;
end Bag;

public class Program
  public static method Main()
    using var b := new Bag();
    b.Add(1);
    b.Add(2);
    b.Reset();
    b.Add(7);
    Console.WriteLine(b.Count());
  end Main;
end Program;
