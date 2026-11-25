//! case: s7_7_9981
//! doc: spec
//! section: 7.7
//! title: `??=` into a non-null auto-property never evaluates an owned chain-call right-hand side
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 28.3.11
//! expect: run
//! output: 500
//!   0
//!   0
//! notes: The admitting twin of s7_7_9980: the property starts at 5, so the assignment does not occur and the
//!   right-hand side is never evaluated: t = 500, no W built.
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
public class H
  public property Count: int? := 5;
  public constructor() end constructor;
end H;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Peek(w: W): int
    return w.V;
  end Peek;
  public static method Go(): int
    var h := new H();
    h.Count ??= C.Peek(C.Make()());
    return h.Count!;
  end Go;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      t := t + C.Go();
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
