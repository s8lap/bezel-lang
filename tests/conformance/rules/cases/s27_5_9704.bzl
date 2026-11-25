//! case: s27_5_9704
//! doc: spec
//! section: 27.5
//! title: a conditional of lambda arms bound to a local, passed and called, 200x
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 28.9, 20
//! expect: run
//! output: 2606
//! notes: Pins #9210 (pick ac402515c): a capture cell is created only on the path that evaluates the
//!   capturing lambda, and the scope-end reclaim of the loop-body local must not free an
//!   uninitialised holder when the capturing arm of a conditional was not taken. The corpus runner
//!   has no sanitizer leg, so the pin is the observable half: the program must run to the end and
//!   print the stated total (the unfixed compiler crashes). Total hand-computed: The local g holds
//!   the selected closure and is read twice: i = 101..199: 5 + 5 = 10, 99 iterations, 990; i =
//!   0..100: 8 + 8 = 16, 101 iterations, 1616; total 2606.
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
      var g := (if i > 100 then lambda() => x else lambda() => 8);
      var f := fl.Get(g);
      var h := g();
      t := t + f + h;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
