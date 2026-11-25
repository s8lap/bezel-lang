//! case: s20_9982
//! doc: spec
//! section: 20
//! title: an `if` arm holding an owned chain-call result is produced only when that arm is selected
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch, the `else`
//!   branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 28.3.11
//! expect: run
//! output: 792
//!   99
//!   99
//! notes: Trace over j in 0..98: 33 multiples of 3 give n = 3 (arm evaluated) and m = 1; the other 66 give n =
//!   7 and m = 3 (arm evaluated). t = 33*4 + 66*10 = 792; W built 33 + 66 = 99, each disposed at the end
//!   of its full expression.
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
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 99 do
      var n := (if j mod 3 = 0 then C.Make()().V else 7);
      var m := (if j mod 3 = 0 then 1 else C.Make()().V);
      t := t + n + m;
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
