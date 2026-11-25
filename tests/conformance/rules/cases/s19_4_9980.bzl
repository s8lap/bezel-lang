//! case: s19_4_9980
//! doc: spec
//! section: 19.4
//! title: an expression-`case` guard and arm holding owned chain-call results each dispose their own value
//! rule: The first arm whose pattern matches *and* whose guard is true is selected.
//! cites: 19.4, 19.2, 28.3.11
//! expect: run
//! output: 300
//!   150
//!   150
//! notes: Pins #9340 (fix c94afc6c5). k = 0: the guard evaluates a W (3, disposed at the guard's own
//!   boundary), the guard is true, the first arm yields a second W: 2 per call. k = 1: the pattern fails
//!   so the guard never runs and the `_` arm yields one W. 50 * 2 + 50 * 1 = 150 built and destroyed; t =
//!   100 * 3.
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
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Go(k: int): owned W
    return (case k of 0 when C.Make()().V = 3 => C.Make()(), _ => C.Make()());
  end Go;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      using var w := C.Go(j mod 2);
      t := t + w.V;
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
