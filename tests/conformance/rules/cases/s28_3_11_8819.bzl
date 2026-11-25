//! case: s28_3_11_8819
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result bound by `using var` is disposed once, by the local
//! rule: A temporary that is instead bound (to a `var`/`using var`), `move`d, returned, or transferred into an owning location is governed by the corresponding transfer or discharge rule and is not disposed under this rule (it would otherwise be disposed twice).
//! cites: 28.3.11, 28.3.10, 28.3.5
//! expect: run
//! output: 600
//!  200
//! notes: Pins #9082 (fix 84e3f112d). `C.Make()()` is a chain call whose OWNED result is bound to `w`; the local alone disposes it. 200 iterations:
//!  t = 3 * 200 = 600 and 200 destructor runs (a second disposal would print 400).
module Conf;
public class W
  public static Drops: int := 0;
  public V: int := 3;
  public constructor() end constructor;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var w := C.Make()();
      t := t + w.V;
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
