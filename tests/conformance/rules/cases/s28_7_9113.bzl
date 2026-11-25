//! case: s28_7_9113
//! doc: spec
//! section: 28.7
//! title: a receiver assigned through its indexer and then returned owned is still readable by the
//!   caller
//! rule: every owned value is disposed exactly once on every path
//! cites: 28.7, 28.3, 23.4
//! expect: run
//! output: 26
//! issue: 9156
//! notes: Pins the observable half of #9156 (pick 917276c6d): an indexer assignment `b[k] := v` on
//!   a local receiver reclaims the receiver at its scope end (the defect was a leak, which the
//!   corpus cannot see: no leak leg). Spec-decided control, green on both arms: the receiver
//!   must stay valid and readable wherever it is still owned. The returned receiver must NOT
//!   be reclaimed at Make's scope end.
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
  static method Make(k: int): owned Box
    var b := new Box();
    b[k] := 5;
    return b;
  end Make;

  public static method Main(): int
    var t := 0;
    for var j in 0..<4 do
      using var x := Make(j);
      t := t + x[0];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
