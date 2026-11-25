//! case: s28_14_8903
//! doc: spec
//! section: 28.14
//! title: a stored closure passed as an indexer's function-typed index argument stays callable after the
//!   read
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9, 23.4
//! expect: run
//! output: 1800 9
//! notes: Harvested from the #9115/#9134 review (r43-rev-m65-9114). `Gen.Stored()` returns a closure kept in
//!   a field and passes it as the index argument of a virtual indexer's getter; the indexer's release
//!   of its call-bounded argument must not free the field's closure. 9 per read (200 reads: 1800), and
//!   `f()` afterwards is still 9. Value-only: the corpus has no leak leg, so this is green on both
//!   arms; it guards the over-release direction.
module Conf;
public class Gen
  private _cb: () -> int;
  public method Stored(): () -> int
    return _cb;
  end Stored;
  public constructor() _cb := lambda() => 9; end constructor;
end Gen;

public class Fl
  private _v: int := 0;
  public virtual property [q: () -> int]: int
    get => _v + q();
    set => do _v := value + q(); end;
  end;
  public constructor() end constructor;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    using var g := new Gen();
    for var j in 0 ..< 200 do
      using var fl := new Fl();
      t := t + fl[g.Stored()];
    end for;
    var f := g.Stored();
    Console.WriteLine($"{t} {f()}");
    return 0;
  end Main;
end Program;
