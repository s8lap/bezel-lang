//! case: s23_4_9103
//! doc: spec
//! section: 23.4
//! title: an indexer assigned and read through an interface and through the class sees one state
//! rule: Indexers may be declared in interfaces with the body-less form
//! cites: 23.4, 11.2
//! expect: run
//! output: 40
//! issue: 9156
//! notes: Pins the observable half of #9156 (pick 917276c6d): an indexer assignment `b[k] := v` on
//!   a local receiver reclaims the receiver at its scope end (the defect was a leak, which the
//!   corpus cannot see: no leak leg). Spec-decided control, green on both arms: the receiver
//!   must stay valid and readable wherever it is still owned. The indexer assignment
//!   dispatches dynamically through the interface; the receiver is reclaimed once, at the
//!   scope end.
module Conf;

public interface IBox
  property [i: int]: int;
end IBox;

public class Box: IBox
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
    for var j in 0..<4 do
      var b := new Box();
      var ib: IBox := b;
      ib[0] := 4;
      b[0] := 5;
      t := t + ib[0] + b[0];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
