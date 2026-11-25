//! case: s20_9980
//! doc: spec
//! section: 20
//! title: a `using var` over an `if` of two owned chain-call arms disposes the selected value once
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch, the `else`
//!   branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 28.3, 28.10
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: Both arms are owned, so the expression is owned (owned arms meet to owned) and `s` is disposed at
//!   the end of each iteration: one W per iteration (only the selected arm runs), t = 100 * 3.
module Conf;
public class W
  public static Made: int := 0;
  public static Drops: int := 0;
  public V: int := 3;
  public constructor()
    W.Made := W.Made + 1;
  end constructor;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      using var s := (if j mod 2 = 0 then C.Make()() else C.Make()());
      t := t + s.V;
    end for;
    return t;
  end Run;
  public static method Main(): int
    var t := C.Run();
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Made}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
