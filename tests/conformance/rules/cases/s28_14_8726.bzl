//! case: s28_14_8726
//! doc: spec
//! section: 28.14
//! title: a closure local used as the index argument of a null-coalescing assignment
//! rule: `x ??= e` assigns `e` only when `x`'s current value is null
//! cites: 28.14, 6.6, 7.7
//! expect: run
//! stdout: 600
//! notes: Pins #9110 (`??=` with a local). `fl[g] ??= 3` stores 3 only when the indexer reads null:
//!   the first statement stores it (the field starts null), the second finds 3 and stores
//!   nothing. The read-back then adds 3 per iteration, 200 iterations: 600.
module Conf;
public class FlN
  private _v: int? := null;
  public property [q: () -> int]: int?
    get => _v;
    set => do
      _v := value;
    end;
  end;
  public constructor() end constructor;
end FlN;
public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var fl := new FlN();
      var g := lambda() => 7;
      fl[g] ??= 3;
      fl[g] ??= 4;
      t := t + (fl[g] ?? 0);
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
