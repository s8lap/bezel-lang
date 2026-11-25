//! case: s27_5_9703
//! doc: spec
//! section: 27.5
//! title: case-expression lambda arms, one capturing, run in a 200x loop
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 28.9, 20
//! expect: run
//! output: 1465
//! notes: Pins #9210 (pick ac402515c): a capture cell is created only on the path that evaluates the
//!   capturing lambda, and the scope-end reclaim of the loop-body local must not free an
//!   uninitialised holder when the capturing arm of a conditional was not taken. The corpus runner
//!   has no sanitizer leg, so the pin is the observable half: the program must run to the end and
//!   print the stated total (the unfixed compiler crashes). Total hand-computed: k = i mod 3 over i =
//!   0..199: k = 0 for 67 iterations (the capturing arm, x = 5): 335; k = 1 for 67 iterations: 8,
//!   536; k = 2 for 66 iterations: 9, 594; total 1465.
module Conf;

public class Fl
  public constructor()
  end constructor;
  public method Get(q: () -> int): int
    var v := q();
    return v;
  end Get;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var fl := new Fl();
      var x := 5;
      var k := i mod 3;
      var f := fl.Get((case k of 0 => lambda() => x, 1 => lambda() => 8, _ => lambda() => 9));
      t := t + f;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
