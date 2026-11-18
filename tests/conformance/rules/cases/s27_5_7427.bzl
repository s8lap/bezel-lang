//! case: s27_5_7427
//! doc: spec
//! section: 27.5
//! title: a closure reassigning a captured owned class binding is rejected
//! rule: the lambda may not assign to it, transfer from it, or dispose it
//! cites: 27.5, 28.3
//! expect: reject
//! diagnostic: cannot assign to captured owned binding
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
    var b := new Box(1); var f2 := lambda() => lambda() => do b := new Box(5); return b.V; end;
    var r := f2()(); return r + b.V;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
