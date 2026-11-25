//! case: s28_3_11_9206
//! doc: spec
//! section: 28.3.11
//! title: an async iterator body disposes an unbound owned chain-call result once per yield
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 29.8, 30.2
//! expect: run
//! output: 630
//!  210
//! notes: Pins #9141 (fix 658bf14cc), the async spelling of the 8825 sync twin. Gen yields 3 per iteration and
//!   builds one temporary per yield; 70 drains: t = 70 * 3 * 3 = 630, Drops = 210.
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
  public static async method Gen(): IAsyncEnumerable<int>
    for var k in 0 ..< 3 do
      var n := C.Make()().V;
      await Task.Delay(0);
      yield return n;
    end for;
  end Gen;
  public static async method Main(): Task<int>
    var t := 0;
    for var j in 0 ..< 70 do
      await for var x in C.Gen() do
        t := t + x;
      end for;
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
