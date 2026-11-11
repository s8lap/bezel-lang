//! case: s14_5_4504
//! doc: spec
//! section: 14.5
//! title: a null-conditional call of a generic class method infers R; a null receiver
//!   skips the call
//! rule: If any segment of a chain is null, the remainder of the chain is skipped on
//!   the same terms and the whole chain's result is `null`
//! cites: 14.5
//! expect: run
//! output: 129
//! issue: 8527
module Conf;

public class Box
  public Age: int;
  public constructor(a: int) Age := a; end constructor;
end Box;

public record Pt(X: int, Y: int);

public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;

public class Gen<U>
  public V: U;
  public constructor(v: U) V := v; end constructor;
  public method Map<R>(f: (U) -> R): R
    return f(V);
  end Map;
end Gen;

public class Program
  public static method Main()
    var n: Helper? := null;
    var g: Gen<int>? := new Gen<int>(10);
    var gr: int := g?.Map(lambda(u: int) => u * 3) ?? -1;
    var q2: int := n?.Ap(5, lambda(v: int) => v * 2) ?? 99;
    Console.WriteLine(gr + q2);
  end Main;
end Program;
