//! case: s28_7_9114
//! doc: spec
//! section: 28.7
//! title: a receiver assigned through its indexer and then moved into an OwningArray is still
//!   readable there
//! rule: every owned value is disposed exactly once on every path
//! cites: 28.7, 28.3, 4.5
//! expect: run
//! output: 26
//! issue: 9156
//! notes: Pins the observable half of #9156 (pick 917276c6d): an indexer assignment `b[k] := v` on
//!   a local receiver reclaims the receiver at its scope end (the defect was a leak, which the
//!   corpus cannot see: no leak leg). Spec-decided control, green on both arms: the receiver
//!   must stay valid and readable wherever it is still owned. A moved receiver belongs to the
//!   collection, so the scope end must not reclaim it.
module Conf;

public class Box
  private _v: int := 0;
  public property [i: int]: int
    get => _v;
    set => do _v := value + i; end;
  end;
  public constructor() end constructor;
end Box;

public class Program
  public static method Main(): int
    var t := 0;
    using var bag := new OwningArray<Box>();
    for var j in 0..<4 do
      var b := new Box();
      b[j] := 5;
      bag.Add(move b);
    end;
    for var x in bag do t := t + x[0]; end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
