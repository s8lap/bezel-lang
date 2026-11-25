//! case: s28_3_11_9203
//! doc: spec
//! section: 28.3.11
//! title: an unbound owned chain-call result on a skipped `and` arm is never produced nor disposed
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10, 30.2
//! expect: run
//! output: 600
//!  100
//! notes: Pins #9141 (fix 658bf14cc), the async spelling of the 8825 sync twin. Only the even-j calls evaluate
//!   the right operand of `and`, so 100 temporaries are produced and disposed: Drops = 100; t = 3 * 200 =
//!   600.
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
    var b := j mod 2 = 0 and C.Make()().V = 3;
    await Task.Delay(0);
    if b then
      return 3;
    end if;
    return 3;
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
