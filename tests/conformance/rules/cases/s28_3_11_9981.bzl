//! case: s28_3_11_9981
//! doc: spec
//! section: 28.3.11
//! title: an owned chain-call result in an `and` right operand is disposed on unwind when a later use throws
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during unwinding
//! cites: 28.3.11, 31.1
//! expect: run
//! output: 550
//!   50
//!   50
//! notes: Trace: odd j skips the right operand (t+1, 50 times). Even j produces a W, `.Boom()` throws, the
//!   temporary is disposed on the unwind and the handler runs (t+10, 50 times). t = 50 + 500 = 550; W
//!   built 50, destroyed 50.
module Conf;
public class W
  public static Made: int := 0;
  public static Drops: int := 0;
  public V: int := 3;
  public constructor()
    W.Made := W.Made + 1;
  end constructor;
  public method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
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
      try
        var b := j mod 2 = 0 and C.Make()().Boom() = 3;
        t := t + 1;
      catch
        InvalidOperationException => do t := t + 10; end,
      end try;
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
