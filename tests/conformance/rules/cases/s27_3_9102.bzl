//! case: s27_3_9102
//! doc: spec
//! section: 27.3
//! title: a unit-bodied lambda picks the `() -> ()` slot over an `Object` slot and over a nullable
//!   `int?` slot
//! rule: Expression-bodied lambdas need no inference rule: the body expression's type is the return
//!   type, and a body of type `()` makes a procedure lambda.
//! cites: 27.3, 22.6
//! expect: run
//! output: U
//!   U
//! issue: 8784
//! notes: Pins #8784/#8821 (picks 8aa16b5ee, e50388b16): an expression-bodied lambda whose body has
//!   type `()` is a procedure lambda and selects the `() -> ()` slot at an overload set, where
//!   22.6 ranks a unit-typed body exact and a value body converting there. The overload that
//!   prints is the verdict; the lambda is never called.
module Conf;

public static class KO
  public static method M(f: () -> Object) Console.WriteLine("O"); end;
  public static method M(f: () -> ()) Console.WriteLine("U"); end;
end;

public static class KN
  public static method M(f: () -> int?) Console.WriteLine("N"); end;
  public static method M(f: () -> ()) Console.WriteLine("U"); end;
end;

public static class Program
  public static method Main(): int
    KO.M(lambda() => Console.WriteLine("x"));
    KN.M(lambda() => Console.WriteLine("x"));
    return 0;
  end;
end;
