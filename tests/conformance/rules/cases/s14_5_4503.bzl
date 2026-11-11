//! case: s14_5_4503
//! doc: spec
//! section: 14.5
//! title: a null-conditional call of a generic method infers R from the lambda's
//!   record result
//! rule: the lambda's inferred return type then unifies against the function type's
//!   return position, binding type parameters still free
//! cites: 7.5
//! expect: run
//! output: 7
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

public class Program
  public static method Main()
    var h: Helper? := new Helper();
    var r: Pt? := h?.Ap(3, lambda(v: int) => new Pt(v, v + 1));
    Console.WriteLine(r!.X + r!.Y);
  end Main;
end Program;
