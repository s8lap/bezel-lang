//! case: s19_2_9980
//! doc: spec
//! section: 19.2
//! title: an expression-`case` of owned chain-call arms stored into an owned field in a loop disposes all
//! rule: the ownership of the `case` result follows the arm-meet rule of
//! cites: 19.2, 28.3, 28.7
//! expect: run
//! output: 300
//!   300
//!   300
//! notes: Pins #9340 (fix c94afc6c5). Both arms are owned, so the field owns the result. Each call builds 3 W
//!   (one per inner iteration): two are dropped on overwrite (28.7) and the last with `h`. 100 calls:
//!   built 300, destroyed 300.
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
  public static method Go(k: int): int
    using var h := new H();
    for var i in 0 ..< 3 do
      h.f := (case k of 0 => C.Make()(), _ => C.Make()());
    end for;
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
