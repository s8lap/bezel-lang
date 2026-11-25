//! case: s27_5_9701
//! doc: spec
//! section: 27.5
//! title: conditional lambda arms capturing one local run in a 200x loop
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 28.9, 20
//! expect: run
//! output: 1101
//! notes: Pins #9210 (pick ac402515c): a capture cell is created only on the path that evaluates the
//!   capturing lambda, and the scope-end reclaim of the loop-body local must not free an
//!   uninitialised holder when the capturing arm of a conditional was not taken. The corpus runner
//!   has no sanitizer leg, so the pin is the observable half: the program must run to the end and
//!   print the stated total (the unfixed compiler crashes). Total hand-computed: i = 101..199 (99
//!   iterations) take the then arm: 99*5 = 495; i = 0..100 (101 iterations) take the else arm: 101*6
//!   = 606; total 1101.
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
      var f := fl.Get((if i > 100 then lambda() => x else lambda() => x + 1));
      t := t + f;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
