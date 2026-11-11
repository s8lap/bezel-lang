//! case: s14_5_4501
//! doc: spec
//! section: 14.5
//! title: a null-conditional call of a generic method infers R from the lambda's
//!   string result
//! rule: the lambda's inferred return type then unifies against the function type's
//!   return position, binding type parameters still free
//! cites: 7.5
//! expect: run
//! output: s5
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
    var s: string := h?.Ap(5, lambda(v: int) => "s" + v.ToString()) ?? "none";
    Console.WriteLine(s);
  end Main;
end Program;
