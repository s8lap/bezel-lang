//! case: s27_5_9707
//! doc: spec
//! section: 27.5
//! title: a variable mutated after a conditional lambda is formed is seen by the call
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5, 28.9
//! expect: run
//! output: 2293
//! notes: Pins #9210's neighbouring shape (pick ac402515c) without a crash claim: Spec-decided control,
//!   green before the fix: the closure shares x's cell, so the later store x := x + 10 is read by the
//!   call: i = 101..199: 15, 99*15 = 1485; i = 0..100: the non-capturing arm 8, 808; total 2293.
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
      x := x + 10;
      var f := fl.Get(g);
      t := t + f;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
