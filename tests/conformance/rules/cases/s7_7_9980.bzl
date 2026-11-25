//! case: s7_7_9980
//! doc: spec
//! section: 7.7
//! title: `??=` into a null auto-property disposes an owned chain-call result in its right-hand side once
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the
//!   assignment occurs
//! cites: 7.7, 28.3.11
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: Pins #9337 (fix 7e40dbefa). The property is null, so the assignment occurs: Peek borrows the unbound
//!   W (3), which is disposed at the end of the statement. 100 calls: t = 300, W built 100, destroyed
//!   100.
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
  public property Count: int? := null;
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
