//! case: s27_5_8111
//! doc: spec
//! section: 27.5
//! title: a closure returned by an immediately-invoked lambda sees later writes to the
//!  captured local
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 23
//! notes: Pins #8922. The inner lambda captures k through the immediately-invoked outer
//!  lambda; k is 20 when f() runs, so 20 + 3 = 23.
module Conf;
public class P
  public static method Run(): int
    var k := 10;
    var f := (lambda(): () -> int => lambda() => k + 3)();
    k := 20;
    return f();
  end Run;
  public static method Main(): int
    Console.WriteLine(P.Run());
    return 0;
  end Main;
end P;
