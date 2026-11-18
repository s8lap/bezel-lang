//! case: s27_5_7426
//! doc: spec
//! section: 27.5
//! title: an escaping closure keeps writing the captured string it shares with its frame
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! output: 1800
module Conf;
public class Box
  public V: int := 0;
  public constructor(v: int)
    V := v;
  end constructor;
end Box;
public class Holder
  public F: () -> int := lambda() => 0;
end Holder;
public class C
  public static method Mk(t: string): () -> int
    var s := t + "b"; var f2 := lambda() => lambda() => do s := s + "c"; return s.Length; end;
    s := s + "x";
    return f2();
  end Mk;
  public static method Run(t: string): int
    var g := C.Mk(t); return g() + g();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
