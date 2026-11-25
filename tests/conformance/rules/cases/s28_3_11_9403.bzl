//! case: s28_3_11_9403
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result passed to a unit method's borrowed parameter is disposed once
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced,
//!   after every use of it within that expression has completed
//! cites: 28.3.11, 22.3
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
  public static method Look(w: W)
    var v := w.V;
  end Look;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      C.Look(C.Make()());
      t := t + 3;
    end for;
    Console.WriteLine(t);
    Console.WriteLine(W.Drops);
    return 0;
  end Main;
end C;
