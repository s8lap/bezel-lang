//! case: s28_3_11_9404
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result returned from an owned-returning method is disposed once by the caller
//! rule: A temporary that is instead bound (to a `var`/`using var`), `move`d, returned, or
//!   transferred into an owning location is governed by the corresponding transfer or discharge
//!   rule and is not disposed under this rule (it would otherwise be disposed twice).
//! cites: 28.3.11, 28.10
//! expect: run
//! output: 600
//!   200
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
  public static method Pass(): owned W => C.Make()();
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var w := C.Pass();
      t := t + w.V;
    end for;
    Console.WriteLine(t);
    Console.WriteLine(W.Drops);
    return 0;
  end Main;
end C;
