//! case: s27_5_8115
//! doc: spec
//! section: 27.5
//! title: a closure captured by an immediately-invoked lambda over a string local sees a
//!  later reassignment
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 4
//! notes: Pins #8922 over a string capture: k is "hey" when f runs, so f() is "hey!" of
//!  length 4.
module Conf;
public class P
  public static method Run(): int
    var k := "hello";
    var f := (lambda(): () -> string => lambda() => k + "!")();
    k := "hey";
    return f().Length;
  end Run;
  public static method Main(): int
    Console.WriteLine(P.Run());
    return 0;
  end Main;
end P;
