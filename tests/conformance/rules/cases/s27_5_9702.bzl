//! case: s27_5_9702
//! doc: spec
//! section: 27.5
//! title: nested conditional lambda arms, some capturing, run in a 200x loop
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 28.9, 20
//! expect: run
//! output: 848
//! notes: Pins #9210 (pick ac402515c): a capture cell is created only on the path that evaluates the
//!   capturing lambda, and the scope-end reclaim of the loop-body local must not free an
//!   uninitialised holder when the capturing arm of a conditional was not taken. The corpus runner
//!   has no sanitizer leg, so the pin is the observable half: the program must run to the end and
//!   print the stated total (the unfixed compiler crashes). Total hand-computed: i = 151..199: x = 5,
//!   49*5 = 245; i = 101..150: 2, 50*2 = 100; i = 51..100: y = 7, 50*7 = 350; i = 0..50: 3, 51*3 =
//!   153; total 848.
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
      var y := 7;
      var f := fl.Get((if i > 100 then (if i > 150 then lambda() => x else lambda() => 2) else (if i > 50 then lambda() => y else lambda() => 3)));
      t := t + f;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
