//! case: s7_7_9901
//! doc: spec
//! section: 7.7
//! title: ??= on a non-null owned local never evaluates its owned chain-call right-hand side
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 6.6, 28.3
//! expect: run
//! output: 0
//!   1
//! issue: 9177
//! notes: Harvested from the #9177 review (a_local_nonnull). `w` is non-null on every pass, so the
//!   right-hand side `C.Make()()` never runs: Calls stays 0 (7.7: a non-null left skips the right
//!   operand entirely). The one W the local holds is disposed once at scope end (28.3), so Drops is
//!   1.
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
    var w: W? := new W();
    for var j in 0 ..< 200 do
      w ??= C.Make()();
    end for;
  end Run;

  public static method Main()
    Run();
    Console.WriteLine(C.Calls);
    Console.WriteLine(W.Drops);
  end Main;
end C;
