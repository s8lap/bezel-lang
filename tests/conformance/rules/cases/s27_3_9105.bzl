//! case: s27_3_9105
//! doc: spec
//! section: 27.3
//! title: unit-returning Array, Dictionary and TextWriter members as lambda bodies select the `()
//!   -> ()` slot
//! rule: Expression-bodied lambdas need no inference rule: the body expression's type is the return
//!   type, and a body of type `()` makes a procedure lambda.
//! cites: 27.3, 22.6
//! expect: run
//! output: B
//!   B
//!   B
//!   B
//!   B
//! issue: 8784
//! notes: Pins #8784/#8821 (picks 8aa16b5ee, e50388b16): an expression-bodied lambda whose body has
//!   type `()` is a procedure lambda and selects the `() -> ()` slot at an overload set, where
//!   22.6 ranks a unit-typed body exact and a value body converting there. The overload that
//!   prints is the verdict; the lambda is never called.
module Conf;

public static class K
  public static method M(f: () -> string) Console.WriteLine("A"); end;
  public static method M(f: () -> ()) Console.WriteLine("B"); end;
end;

public static class Program
  public static method Main(): int
    var xs := new Array<int>(); xs.Add(1);
    var d := new Dictionary<string, int>();
    K.M(lambda() => xs.Add(2));
    K.M(lambda() => xs.RemoveAt(0));
    K.M(lambda() => xs.Clear());
    K.M(lambda() => d.Add("a", 1));
    K.M(lambda() => Console.Out.WriteLine("x"));
    return 0;
  end;
end;
