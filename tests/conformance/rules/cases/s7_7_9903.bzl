//! case: s7_7_9903
//! doc: spec
//! section: 7.7
//! title: ??= on a null owned local takes the owned chain-call result once per pass and disposes each
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 6.6, 28.3
//! expect: run
//! output: 200
//!   200
//! issue: 9177
//! notes: Harvested from the #9177 review (e_local_null_loopfresh). A fresh null local per pass takes the
//!   assignment, so the right-hand side runs 200 times (Calls 200); each W is owned by its local and
//!   disposed once at the end of its pass (28.3: an owned local is disposed exactly once on every
//!   path), so Drops is 200.
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

  public static method Run()
    for var j in 0 ..< 200 do
      var w: W? := null;
      w ??= C.Make()();
    end for;
  end Run;

  public static method Main()
    Run();
    Console.WriteLine(C.Calls);
    Console.WriteLine(W.Drops);
  end Main;
end C;
