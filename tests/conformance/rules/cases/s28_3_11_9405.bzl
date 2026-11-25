//! case: s28_3_11_9405
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result discarded with the discard form is disposed once
//! rule: `_ := expression;` evaluates `expression` and discharges the disposal obligation once the
//!   statement completes.
//! cites: 17.13, 28.3.11
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
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      _ := C.Make()();
      t := t + 3;
    end for;
    Console.WriteLine(t);
    Console.WriteLine(W.Drops);
    return 0;
  end Main;
end C;
