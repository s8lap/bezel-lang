//! case: s28_3_11_8825
//! doc: spec
//! section: 28.3.11
//! title: an unbound owned chain-call result used as a receiver is disposed once by the temporary rule
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 28.3.5
//! expect: run
//! output: 600
//!  200
//! notes: Pins #9082 (fix 84e3f112d). Admitting control (green on both arms): `C.Make()().V` binds nothing, so the §28.3.11 temporary rule disposes
//!  the W at the end of the full expression: t = 600, 200 disposals.
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
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      t := t + C.Make()().V;
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
