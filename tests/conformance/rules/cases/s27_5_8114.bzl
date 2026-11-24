//! case: s27_5_8114
//! doc: spec
//! section: 27.5
//! title: a closure local initialised from a closure-returning call may be returned after
//!  being captured
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 3
//! notes: Pins #8925. f is captured by u, read through u, then returned; the returned
//!  closure still reads k = 3.
module Conf;
public class P
  public static method Mk(): () -> int
    var k := 3;
    var g: () -> () -> int := lambda(): () -> int => lambda() => k;
    var f: () -> int := g();
    var u := lambda() => f() + 1;
    var r := u();
    return f;
  end Mk;
  public static method Main(): int
    Console.WriteLine(P.Mk()());
    return 0;
  end Main;
end P;
