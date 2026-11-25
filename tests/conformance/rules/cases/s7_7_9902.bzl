//! case: s7_7_9902
//! doc: spec
//! section: 7.7
//! title: ??= on a non-null owned field never evaluates its owned chain-call right-hand side
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 6.6, 28.5
//! expect: run
//! output: 0
//!   1
//! issue: 9177
//! notes: Harvested from the #9177 review (b_field_nonnull). The owned field holds a W throughout, so the
//!   right-hand side never runs (Calls 0) and the one W is disposed once with its holder (Drops 1).
module Conf;

public class W
  public static Drops: int := 0;

  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;

public class C
  public static Calls: int := 0;

  public static method Make(): () -> owned W
    return lambda(): W => do
      C.Calls := C.Calls + 1;
      return new W();
    end;
  end Make;

  public owned F: W? := null;

  public static method Run()
    var c := new C();
    c.F := new W();
    for var j in 0 ..< 200 do
      c.F ??= C.Make()();
    end for;
  end Run;

  public static method Main()
    Run();
    Console.WriteLine(C.Calls);
    Console.WriteLine(W.Drops);
  end Main;
end C;
