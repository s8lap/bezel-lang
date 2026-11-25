//! case: s27_5_9705
//! doc: spec
//! section: 27.5
//! title: a callee that throws after a conditional lambda arm is called, 200x
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 28.9, 31.1
//! expect: run
//! output: 99
//! notes: Pins #9210 (pick ac402515c): a capture cell is created only on the path that evaluates the
//!   capturing lambda, and the scope-end reclaim of the loop-body local must not free an
//!   uninitialised holder when the capturing arm of a conditional was not taken. The corpus runner
//!   has no sanitizer leg, so the pin is the observable half: the program must run to the end and
//!   print the stated total (the unfixed compiler crashes). Total hand-computed: i = 101..199 take
//!   the capturing arm: the callee reads 5 > 0 and throws, the arm adds 1 (99 iterations); i = 0..100
//!   take the arm returning 0, nothing is added; total 99. The unwind passes through the same scope-
//!   end reclaim.
module Conf;

public class Fl
  public constructor()
  end constructor;
  public method Get(q: () -> int): int
    var v := q();
    return v;
  end Get;
  public method Boom(q: () -> int): int
    var v := q();
    if v > 0 then
      throw new InvalidOperationException("boom");
    end if;
    return v;
  end Boom;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var fl := new Fl();
      var x := 5;
      try
        var r := fl.Boom((if i > 100 then lambda() => x else lambda() => 0));
        t := t + r;
      catch
        InvalidOperationException => t := t + 1;
      end try;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
