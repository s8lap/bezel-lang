//! case: s27_5_8112
//! doc: spec
//! section: 27.5
//! title: two closures minted by one lambda called twice both see a later write to the
//!  shared captured local
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 8
//! notes: Pins #8922. a and b share the one cell of k; k is 1 when they run: 4 + 4 = 8.
module Conf;
public class P
  public static method Run(): int
    var k := 10;
    var outer := lambda(): () -> int => do
      var f := (lambda(): () -> int => lambda() => k + 3)();
      return f;
    end;
    var a := outer();
    var b := outer();
    k := 1;
    return a() + b();
  end Run;
  public static method Main(): int
    Console.WriteLine(P.Run());
    return 0;
  end Main;
end P;
