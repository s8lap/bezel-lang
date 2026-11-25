//! case: s7_7_9984
//! doc: spec
//! section: 7.7
//! title: `??` over an owned chain-call left operand that yields a value skips its `new` right operand
//! rule: when the left is non-null the right operand expression does not run at all, and none of its calls,
//!   allocations, side effects, or faults occur
//! cites: 7.7, 28.3.11
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: The admitting twin of the #9340 shapes (the left chain call was never broken): the left value is
//!   non-null every time, so the `new W()` right operand is skipped; 100 W built, 100 destroyed.
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
  public static method MakeN(): () -> owned W?
    return lambda(): W? => new W();
  end MakeN;
  public static method Go(): owned W
    return C.MakeN()() ?? new W();
  end Go;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      using var w := C.Go();
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
