//! case: s20_9981
//! doc: spec
//! section: 20
//! title: an `if` of a chain-call arm and a `new` arm stored into an owned field disposes every value once
//! rule: owned arms meet to owned
//! cites: 20, 28.3, 28.7
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: Both arms are owned, so the owned field takes either; each store drops the previous value (28.7) and
//!   `h` drops the last one at the end of Run: 100 W built, 100 destroyed.
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
  public owned f: W? := null;
  public constructor() end constructor;
end H;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Run(): int
    var t := 0;
    using var h := new H();
    for var j in 0 ..< 100 do
      h.f := (if j mod 2 = 0 then C.Make()() else new W());
      t := t + h.f!.V;
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
