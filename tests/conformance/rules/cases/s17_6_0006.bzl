//! case: s17_6_0006
//! doc: spec
//! section: 17.6
//! title: an indexer-slot ++ desugars to a read-modify-write through the indexer
//! rule: "For an indexer slot `expr[i]` ... `expr[i]++` desugars to a
//!   read-modify-write through the indexer ... `__coll[__idx] := __coll[__idx]
//!   + 1;`."
//! cites: 17.6
//! expect: run
//! stdout: 1
module Conf;

public class Tally
  private owned _counts: Array<int> := new Array<int>();

  public constructor()
    _counts.Add(0);
  end constructor;

  public property [i: int]: int
    get => _counts[i];
    set => do _counts[i] := value; end;
  end Tally;
end Tally;

public class Program
  public static method Main()
    using var t := new Tally();
    t[0]++;
    Console.WriteLine(t[0]);
  end Main;
end Program;
