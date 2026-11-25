//! case: s28_3_11_9980
//! doc: spec
//! section: 28.3.11
//! title: an evaluated `and`/`or` right operand holding an owned chain-call result is disposed once
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after
//!   every use of it within that expression has completed
//! cites: 28.3.11, 17.2
//! expect: run
//! output: 150
//!   100
//!   100
//! notes: Trace: even j evaluates the `and` right operand (50 W, b true, t+1 each); odd j evaluates the `or`
//!   right operand (50 W); c is true on every j (t+1 each). t = 50 + 100 = 150, W built 100, destroyed
//!   100.
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
    for var j in 0 ..< 100 do
      var b := j mod 2 = 0 and C.Make()().V = 3;
      var c := j mod 2 = 0 or C.Make()().V = 3;
      if b then
        t := t + 1;
      end if;
      if c then
        t := t + 1;
      end if;
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
