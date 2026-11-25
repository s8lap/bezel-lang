//! case: s27_3_9104
//! doc: spec
//! section: 27.3
//! title: a constructor overload set `() -> string` / `() -> ()` selects by the lambda body's type
//! rule: Expression-bodied lambdas need no inference rule: the body expression's type is the return
//!   type, and a body of type `()` makes a procedure lambda.
//! cites: 27.3, 22.6
//! expect: run
//! output: U
//!   S
//! issue: 8784
//! notes: Pins #8784/#8821 (picks 8aa16b5ee, e50388b16): an expression-bodied lambda whose body has
//!   type `()` is a procedure lambda and selects the `() -> ()` slot at an overload set, where
//!   22.6 ranks a unit-typed body exact and a value body converting there. The overload that
//!   prints is the verdict; the lambda is never called.
module Conf;

public class K
  public constructor(f: () -> string) Console.WriteLine("S"); end constructor;
  public constructor(f: () -> ()) Console.WriteLine("U"); end constructor;
end K;

public static class Program
  public static method Main(): int
    var a := new K(lambda() => Console.WriteLine("x"));
    var b := new K(lambda() => "s");
    return 0;
  end;
end;
