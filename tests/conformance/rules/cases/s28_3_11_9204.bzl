//! case: s28_3_11_9204
//! doc: spec
//! section: 28.3.11
//! title: an async loop with continue, break and await disposes each chain-call temporary once
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 30.2
//! expect: run
//! output: 600
//!  600
//! notes: Pins #9141 (fix 658bf14cc), the async spelling of the 8825 sync twin. Per Step the loop body
//!   evaluates Make on k = 0, 2 and 3 (k = 1 continues), so 3 temporaries, each disposed once: Drops =
//!   600; t = 3 * 200 = 600.
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
    var n := 0;
    for var k in 0 ..< 4 do
      if k = 1 then
        continue;
      end if;
      n := C.Make()().V;
      await Task.Delay(0);
      if k = 3 then
        break;
      end if;
    end for;
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
