//! case: s28_3_11_8514
//! doc: spec
//! section: 28.3.11
//! title: two conditional receiver temporaries in one scope, one throwing, are all disposed with
//!   none leaked
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during
//!   unwinding by the same implicit `try`/`finally` mechanism as named locals
//! cites: 28.3.11, 28.3.10
//! expect: run
//! stdout: k=200 N=167 Made=167
//! notes: Pins #9009. `Make()` returns an owning choice whose payload `Res` has a destructor;
//!   `Res.Made` counts constructions and `Res.N` destructor runs, so every produced receiver
//!   temporary must be disposed exactly once: N = Made. The static counters draw the
//!   expected §22.12 rule 2 declaration-site warning. Each of the 200 iterations throws
//!   exactly once, so k = 200. Even i: `b` produces a receiver and `Boom` throws (100 Made);
//!   the later lines do not run. Odd i: `b` short-circuits; `c` produces a receiver unless i
//!   mod 3 = 0 (the 33 odd multiples of 3 below 200 skip it), whose Count() = 7 > 5
//!   completes normally, and then `Arg` throws: 100 - 33 = 67 more. Made = 167 and N = 167.
module Conf;
class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Res.Made := Res.Made + 1; end constructor;
  destructor Res.N := Res.N + 1; end destructor;
  public method Id(): int => 7;
end Res;
choice OChoice Text(owned r: Res), Empty;
  public method Count(): int => (case this of Text(r) => r.Id(), Empty => 0);
  public method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
  public method Add(k: int): int => (case this of Text(r) => r.Id() + k, Empty => k);
end OChoice;
class H
  public constructor() end constructor;
  public method Make(): OChoice return OChoice.Text(new Res()); end Make;
  public method Arg(): int
    throw new InvalidOperationException("argboom");
  end Arg;
end H;

class Program
  public static method Main()
    var i := 0; var h := new H(); var k := 0;
    while i < 200 do
      try
        var b := (i mod 2 = 0) and h.Make().Boom() > 0;
        var c := (i mod 3 = 0) or h.Make().Count() > 5;
        h.Arg();
      catch InvalidOperationException => k := k + 1;
      end try;
      i := i + 1;
    end while;
    Console.WriteLine($"k={k} N={Res.N} Made={Res.Made}");
  end Main;
end Program;
