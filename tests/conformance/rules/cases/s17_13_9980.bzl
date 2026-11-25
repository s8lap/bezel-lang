//! case: s17_13_9980
//! doc: spec
//! section: 17.13
//! title: a discarded `if` of two owned chain-call arms disposes the selected value at statement end
//! rule: An owned result is disposed at the end of the discard statement.
//! cites: 17.13, 20, 28.3.11
//! expect: run
//! output: 300
//!   100
//!   100
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
      _ := (if j mod 2 = 0 then C.Make()() else C.Make()());
      t := t + 3;
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
