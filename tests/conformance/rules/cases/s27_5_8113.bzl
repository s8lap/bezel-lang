//! case: s27_5_8113
//! doc: spec
//! section: 27.5
//! title: an expression-bodied lambda that calls a closure-returning lambda, called twice,
//!  sees a later write
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 12
//! notes: Pins #8921. Both closures returned through g share k's cell; k is 6 when they
//!  run: 6 + 6 = 12.
module Conf;
public class P
  public static method Run(): int
    var k := 5;
    var h: () -> () -> int := lambda(): () -> int => lambda() => k;
    var g: () -> () -> int := lambda(): () -> int => h();
    var f1 := g();
    var f2 := g();
    k := 6;
    return f1() + f2();
  end Run;
  public static method Main(): int
    Console.WriteLine(P.Run());
    return 0;
  end Main;
end P;
