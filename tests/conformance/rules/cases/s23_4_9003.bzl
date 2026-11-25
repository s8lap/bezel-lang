//! case: s23_4_9003
//! doc: spec
//! section: 23.4
//! title: a sealed indexer override with an empty setter is dispatched to via base and derived types
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: 10
//!   10
//! notes: Harvested from the #9152 review (q02). Trace: Fl2 seals an override whose getter is `_v + 10` and
//!   whose setter body is empty. Through an `Fl`-typed receiver holding an Fl3, `a[0] := 5` runs Fl2's
//!   empty setter (the base setter would store 5), so `_v` stays 0 and `a[0]` reads 0 + 10 = 10. Through
//!   an `Fl2`-typed receiver, `b[1] := 7` is the same no-op and `b[1]` reads 10.
module Conf;
public class Fl
  protected _v: int := 0;
  public virtual property [i: int]: int
    get => _v;
    set => do _v := value; end;
  end;
  public constructor() end constructor;
end Fl;
public class Fl2 : Fl
  public sealed override property [i: int]: int
    get => _v + 10;
    set => do end;
  end;
  public constructor() end constructor;
end Fl2;
public class Fl3 : Fl2
  public constructor() end constructor;
end Fl3;
public class Program
  public static method Main(): int
    using var a: Fl := new Fl3();
    a[0] := 5;
    Console.WriteLine(a[0]);
    using var b: Fl2 := new Fl3();
    b[1] := 7;
    Console.WriteLine(b[1]);
    return 0;
  end Main;
end Program;
