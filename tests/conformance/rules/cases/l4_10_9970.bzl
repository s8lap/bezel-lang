//! case: l4_10_9970
//! doc: stdlib
//! section: 4.10
//! title: a Set<Cell?> over a class with user equality collapses an equal element and holds one null
//! rule: public method TryAdd(item: T): bool; // returns whether added
//! cites: 4.10, 3.6, 8.4
//! expect: run
//! issue: 9277
//! output: True
//!   False
//!   True
//!   False
//!   2
//!   True
module Conf;

public class Cell
  private _key: int;

  public constructor(key: int)
    _key := key;
  end constructor;

  public override method Equals(other: Object?): bool
    var c := other as Cell;
    if c = null then
      return false;
    end;
    return c._key = _key;
  end Equals;

  public override method GetHashCode(): int => _key;
end Cell;

public class Program
  public static method Main()
    using var a := new Cell(1);
    using var b := new Cell(1);
    var s := new Set<Cell?>();
    Console.WriteLine(s.TryAdd(a));
    Console.WriteLine(s.TryAdd(b));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.Count);
    Console.WriteLine(s.Contains(b));
  end Main;
end Program;
