//! case: s28_14_8905
//! doc: spec
//! section: 28.14
//! title: stored, returned and fresh closures as `??=`, `:=` and read index arguments stay intact
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9, 23.4
//! expect: run
//! output: 9600
//! notes: Harvested from the #9134 review (`??=` discharge of the pinned index closure). Per iteration the
//!   fresh `Fl` starts null; `fl[keep] ??= 1` stores 1, the later `??=` forms find a value and store
//!   nothing, `fl[Gen.Chain(g)] := 3` stores 3, so the reads give 3 and 3, and `keep()` still returns
//!   42: 48 per iteration, 200 iterations: 9600. Value-only control (green on both arms).
module Conf;
public class Gen
  private _cb: () -> int;
  private _k: int := 42;
  public method Pick(flag: bool): () -> int
    if flag then return _cb; end if;
    var k := _k;
    return lambda() => k;
  end Pick;
  public static method Chain(g: Gen): () -> int
    return g.Pick(false);
  end Chain;
  public constructor() _cb := lambda() => 9; end constructor;
end Gen;

public class Fl
  private _v: int? := null;
  public virtual property [q: () -> int]: int?
    get => _v;
    set => do _v := value; end;
  end;
  public constructor() end constructor;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    using var g := new Gen();
    var keep := g.Pick(false);
    for var j in 0 ..< 200 do
      using var fl := new Fl();
      fl[keep] ??= 1;
      fl[g.Pick(true)] ??= 2;
      fl[g.Pick(false)] ??= 2;
      fl[Gen.Chain(g)] := 3;
      fl[lambda() => 4] ??= 2;
      t := t + (fl[g.Pick(true)] ?? 0) + (fl[Gen.Chain(g)] ?? 0) + keep();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
