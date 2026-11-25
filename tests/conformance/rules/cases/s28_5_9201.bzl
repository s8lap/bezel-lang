//! case: s28_5_9201
//! doc: spec
//! section: 28.5
//! title: an `owned` `Dictionary<int, Entry>` field with record values leaves a borrowed class alive
//! rule: **Auto-disposes** the field when the containing object is disposed
//! cites: 28.9, 28.11
//! expect: run
//! output: 800
//!  5
//! notes: Pins #9183 (fix f9f8a62cf): an `owned` Dictionary field with a RECORD value frees the value box at
//!   the holder disposal. The case is an output-only control (green on both arms): the corpus has no leak
//!   leg, so it guards the fix against double free, use-after-free and over-freeing, not the leak itself.
//!   200 holders each Put keys 1, 2, 1 (overwrite) and read Sum = 1 + 2 = 3, TryIt(2) = 2, TryIt(9) = -1:
//!   4 per iteration, 800. `n` (borrowed by the records) is still readable after every holder died: 5.
module Conf;
public class Node
  public V: int;
  public constructor(v: int) V := v; end;
end Node;
public record Entry(Id: int, Owner: Node?);
public class Holder
  private owned _d: Dictionary<int, Entry> := new Dictionary<int, Entry>();
  public method Put(k: int, n: Node) => _d[k] := new Entry(k, n);
  public method Sum(): int
    var s := 0;
    for var kv in _d do
      s := s + kv.Value.Id;
    end;
    return s;
  end Sum;
  public method TryIt(k: int): int
    var e: Entry? := null;
    if _d.TryGetValue(k, out e) then return e!.Id; end;
    return -1;
  end TryIt;
end Holder;
public class Program
  public static method Main()
    using var n := new Node(5);
    var i := 0;
    var t := 0;
    while i < 200 do
      using var h := new Holder();
      h.Put(1, n); h.Put(2, n); h.Put(1, n);
      t := t + h.Sum() + h.TryIt(2) + h.TryIt(9);
      i := i + 1;
    end;
    Console.WriteLine(t);
    Console.WriteLine(n.V);
  end Main;
end Program;
