//! case: s17_15_7428
//! doc: spec
//! section: 17.15
//! title: a lambda assigning a binding-atom binding it captures is rejected
//! rule: a binding is **not an assignable location**: assignment, compound assignment, `++`/`--`, and
//!   binding to a `ref` or `out` parameter are compile errors naming this rule
//! cites: 17.15
//! expect: reject
//! diagnostic: cannot assign to binding-atom binding
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
    var f2 := lambda() => (var u := t + "b"; (var g := lambda() => do u := u + "c"; return u.Length; end; g()));
    return f2();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
