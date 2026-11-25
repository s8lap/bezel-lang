//! case: s7_7_9982
//! doc: spec
//! section: 7.7
//! title: a right-nested `??` chain evaluates its owned operands only while the left stays null
//! rule: in `a ?? b ?? c` a non-null `a` skips both `b` and `c`
//! cites: 7.7, 28.3.11
//! expect: run
//! output: 300
//!   100
//!   100
//! notes: Pins #9340 (fix c94afc6c5). k = 1: the first MN returns a W and both right operands are skipped. k =
//!   0: MN returns null twice and Make produces the W. Exactly one W per call: built 100, destroyed 100.
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
  public static method MN(k: int): owned W?
    if k = 0 then
      return null;
    end if;
    return new W();
  end MN;
  public static method Go(k: int): owned W
    return C.MN(k) ?? (C.MN(k) ?? C.Make()());
  end Go;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      using var w := C.Go(j mod 2);
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
