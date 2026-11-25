//! case: s28_3_11_9201
//! doc: spec
//! section: 28.3.11
//! title: an async body disposes an unbound owned chain-call result once, on both return paths
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 30.2
//! expect: run
//! output: 600
//!  200
//! notes: Pins #9141 (fix 658bf14cc), the async spelling of the 8825 sync twin. `C.Make()().V` binds nothing,
//!   so the W is a temporary disposed at the end of the full expression. Step makes one W per call: t =
//!   200 * 3 = 600, W.Drops = 200 (once per call, on both the even early-return and the odd post-await
//!   path).
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
  public static async method Step(j: int): Task<int>
    var n := C.Make()().V;
    if j mod 2 = 0 then
      return n;
    end if;
    await Task.Delay(0);
    return n;
  end Step;
  public static async method Main(): Task<int>
    var t := 0;
    for var j in 0 ..< 200 do
      t := t + await C.Step(j);
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
