//! case: s17_6_0007
//! doc: spec
//! section: 17.6
//! title: an indexer-slot ++'s index expression is evaluated exactly once
//! rule: "Each subexpression in the LHS — the collection expression and every
//!   index expression — is evaluated exactly once. The indexer's getter and
//!   setter are each called once."
//! cites: 17.6
//! expect: run
//! stdout: 1
module Conf;

public class Tally
  private owned _counts: Array<int> := new Array<int>();

  public constructor()
    _counts.Add(0);
    _counts.Add(0);
  end constructor;

  public property [i: int]: int
    get => _counts[i];
    set => do _counts[i] := value; end;
  end Tally;
end Tally;

public class Program
  private static _evalCount: int := 0;

  public static method Idx(): int
    _evalCount := _evalCount + 1;
    return 0;
  end Idx;

  public static method Main()
    using var t := new Tally();
    t[Idx()]++;
    Console.WriteLine(_evalCount);
  end Main;
end Program;
