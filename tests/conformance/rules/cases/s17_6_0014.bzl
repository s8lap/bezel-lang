//! case: s17_6_0014
//! doc: spec
//! section: 17.6
//! title: an indexer-slot ++ calls the getter once and the setter once
//! rule: "Implementations may not deduplicate the getter+setter pair: every
//!   `arr[i]++` performs one logical read followed by one logical write
//!   through the indexer."
//! cites: 17.6, 6.6
//! expect: run
//! output: get
//!   set
module Conf;

public class Tally
  private owned _counts: Array<int> := new Array<int>();

  public constructor()
    _counts.Add(0);
  end constructor;

  public property [i: int]: int
    get => do
      Console.WriteLine("get");
      return _counts[i];
    end;
    set => do
      Console.WriteLine("set");
      _counts[i] := value;
    end;
  end;
end Tally;

public class Program
  public static method Main()
    using var t := new Tally();
    t[0]++;
  end Main;
end Program;
