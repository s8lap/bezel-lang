//! case: s27_5_7425
//! doc: spec
//! section: 27.5
//! title: a nested lambda formed per loop iteration writes the one shared captured string
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! output: 3400
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

  public static method Run(t: string): int
    var s := t + "b"; var f2 := lambda() => do var acc := 0; for var i in 0..<3 do var g := lambda() => do s := s + "c"; return s.Length; end; acc := acc + g(); end; return acc; end;
    var r := f2(); return r + s.Length;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
