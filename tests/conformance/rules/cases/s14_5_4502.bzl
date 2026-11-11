//! case: s14_5_4502
//! doc: spec
//! section: 14.5
//! title: a null-conditional call of a generic method infers R from the lambda's class
//!   result
//! rule: the lambda's inferred return type then unifies against the function type's
//!   return position, binding type parameters still free
//! cites: 7.5
//! expect: run
//! output: 14
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
    var b: Box? := h?.Ap(7, lambda(v: int) => new Box(v * 2));
    Console.WriteLine(b!.Age);
  end Main;
end Program;
