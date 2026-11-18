//! case: s27_5_7429
//! doc: spec
//! section: 27.5
//! title: an immediately-invoked lambda inside a lambda reads the captured string local
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5, 28.9
//! expect: run
//! output: 1200
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
    var s := t + "b"; var o := lambda() => (lambda() => s.Length)() + s.Length; return o() + s.Length;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
