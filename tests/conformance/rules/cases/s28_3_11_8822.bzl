//! case: s28_3_11_8822
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result bound by `using var` is disposed once when a later call throws
//! rule: A temporary that is instead bound (to a `var`/`using var`), `move`d, returned, or transferred into an owning location is governed by the corresponding transfer or discharge rule and is not disposed under this rule (it would otherwise be disposed twice).
//! cites: 28.3.11, 28.3.10, 28.3.5
//! expect: run
//! output: 600
//!  200
//! notes: Pins #9082 (fix 84e3f112d). After the binding and one read, `Boom` throws on every iteration; the unwind disposes `w` once (the
//!  bound temporary is not also disposed under the temporary rule). t = 3 * 200 = 600, 200 disposals.
module Conf;
public class W
  public static Drops: int := 0;
  public V: int := 3;
  public constructor() end constructor;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Boom(w: W, j: int): int
    if j >= 0 then
      throw new InvalidOperationException("x");
    end if;
    return w.V;
  end Boom;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      try
        using var w := C.Make()();
        t := t + w.V;
        t := t + C.Boom(w, j);
      catch InvalidOperationException =>
        var c := 0;
      end try;
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
