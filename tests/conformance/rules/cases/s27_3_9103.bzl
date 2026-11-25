//! case: s27_3_9103
//! doc: spec
//! section: 27.3
//! title: an instance-method overload set `() -> string` / `() -> ()` selects by the lambda body's
//!   type
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
  public method M(f: () -> string) Console.WriteLine("S"); end;
  public method M(f: () -> ()) Console.WriteLine("U"); end;
end;

public static class Program
  public static method Main(): int
    var k := new K();
    k.M(lambda() => Console.WriteLine("x"));
    k.M(lambda() => "s");
    return 0;
  end;
end;
