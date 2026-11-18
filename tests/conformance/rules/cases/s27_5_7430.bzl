//! case: s27_5_7430
//! doc: spec
//! section: 27.5
//! title: an immediately-invoked lambda captures a using var binding by read
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5, 28.9
//! expect: run
//! output: 800
module Conf;
public class Box
  public V: int := 0;
  public constructor(v: int)
    V := v;
  end constructor;
end Box;
public class Res
  public V: int := 0;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor
  end destructor;
end Res;
public class C
  public static method Run(t: string): int
    using var b := new Box(t.Length + 1); var r := (lambda() => b.V)(); return r + b.V;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
