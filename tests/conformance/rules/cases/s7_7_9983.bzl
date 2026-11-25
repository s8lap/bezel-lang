//! case: s7_7_9983
//! doc: spec
//! section: 7.7
//! title: `??` with an owned chain-call right operand stored into an owned field disposes one value each time
//! rule: the right operand is evaluated only when the left operand's value is `null`
//! cites: 7.7, 28.3.11, 28.7
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: Pins #9340 (fix c94afc6c5). k = 1 stores MN's W and never runs Make; k = 0 stores Make's W. One W
//!   per call, disposed with `h`.
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
  public static method MN(k: int): owned W?
    if k = 0 then
      return null;
    end if;
    return new W();
  end MN;
  public static method Go(k: int): int
    using var h := new H();
    h.f := C.MN(k) ?? C.Make()();
    return h.f!.V;
  end Go;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      t := t + C.Go(j mod 2);
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
