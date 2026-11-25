//! case: s28_3_11_9406
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result transferred into an owned parameter is disposed once, by the callee
//! rule: A temporary that is instead bound (to a `var`/`using var`), `move`d, returned, or
//!   transferred into an owning location is governed by the corresponding transfer or discharge
//!   rule and is not disposed under this rule (it would otherwise be disposed twice).
//! cites: 28.3.11, 22.3
//! expect: run
//! output: 600
//!   200
//! issue: 9117
module Conf;

public class W
  public static Drops: int := 0;
  public V: int := 3;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Take(owned w: W): int
    var v := w.V;
    _ := w;
    return v;
  end Take;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      t := t + C.Take(C.Make()());
    end for;
    Console.WriteLine(t);
    Console.WriteLine(W.Drops);
    return 0;
  end Main;
end C;
