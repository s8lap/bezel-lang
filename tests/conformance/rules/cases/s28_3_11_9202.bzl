//! case: s28_3_11_9202
//! doc: spec
//! section: 28.3.11
//! title: an async body disposes an unbound owned chain-call result once across a throwing await
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 30.2
//! expect: run
//! output: 600
//!  200
//! notes: Pins #9141 (fix 658bf14cc), the async spelling of the 8825 sync twin. Inner makes the temporary then
//!   awaits Boom, which throws on every third j; the exception edge and the normal edge each dispose the
//!   W once. t = 600 (3 per Step: the value or the catch), Drops = 200.
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
  public static async method Boom(j: int): Task<int>
    await Task.Delay(0);
    if j mod 3 = 0 then
      throw new InvalidOperationException("boom");
    end if;
    return 0;
  end Boom;
  public static async method Inner(j: int): Task<int>
    var n := C.Make()().V;
    var z := await C.Boom(j);
    return n + z;
  end Inner;
  public static async method Step(j: int): Task<int>
    try
      return await C.Inner(j);
    catch ex: InvalidOperationException =>
      return 3;
    end try;
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
