//! case: s27_3_9106
//! doc: spec
//! section: 27.3
//! title: a unit-bodied lambda selects the non-generic `() -> ()` slot over a generic `() -> T`
//!   slot
//! rule: Expression-bodied lambdas need no inference rule: the body expression's type is the return
//!   type, and a body of type `()` makes a procedure lambda.
//! cites: 27.3, 22.6, 14.5
//! expect: run
//! output: B
//! issue: 8784
//! notes: Pins #8784/#8821 (picks 8aa16b5ee, e50388b16): an expression-bodied lambda whose body has
//!   type `()` is a procedure lambda and selects the `() -> ()` slot at an overload set, where
//!   22.6 ranks a unit-typed body exact and a value body converting there. The overload that
//!   prints is the verdict; the lambda is never called.
module Conf;

public static class K
  public static method M<T>(f: () -> T) Console.WriteLine("G"); end;
  public static method M(f: () -> ()) Console.WriteLine("B"); end;
end;

public static class Program
  public static method Main(): int
    K.M(lambda() => Console.WriteLine("x"));
    return 0;
  end;
end;
