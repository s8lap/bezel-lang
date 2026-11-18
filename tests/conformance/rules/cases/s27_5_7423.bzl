//! case: s27_5_7423
//! doc: spec
//! section: 27.5
//! title: a nested lambda reads the string local its enclosing lambda just wrote
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! output: 1200
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
    var s := t + "b"; var f2 := lambda() => do s := s + "c"; var g := lambda() => s.Length; return g(); end;
    var r := f2(); return r + s.Length;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
