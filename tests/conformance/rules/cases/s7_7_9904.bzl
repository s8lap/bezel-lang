//! case: s7_7_9904
//! doc: spec
//! section: 7.7
//! title: a second ??= on an owned field short-circuits and each object disposes its one W
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 6.6, 28.5
//! expect: run
//! output: 200
//!   200
//! issue: 9177
//! notes: Harvested from the #9177 review (f_field_null). The first `??=` per object fills the null field,
//!   the second finds it non-null and skips its right-hand side, so Calls is 200 and not 400. Each
//!   object disposes its one W with itself, so Drops is 200.
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
    for var j in 0 ..< 200 do
      var c := new C();
      c.F ??= C.Make()();
      c.F ??= C.Make()();
    end for;
  end Run;

  public static method Main()
    Run();
    Console.WriteLine(C.Calls);
    Console.WriteLine(W.Drops);
  end Main;
end C;
