//! case: s28_3_11_8516
//! doc: spec
//! section: 28.3.11
//! title: a receiver temporary whose producing subexpression never ran is never disposed
//! rule: a temporary whose producing subexpression never ran was never produced
//! cites: 28.3.11, 28.3.10
//! expect: run
//! stdout: N=0 Made=0
//! notes: Pins #9009. `Make()` returns an owning choice whose payload `Res` has a destructor;
//!   `Res.Made` counts constructions and `Res.N` destructor runs, so every produced receiver
//!   temporary must be disposed exactly once: N = Made. The static counters draw the
//!   expected §22.12 rule 2 declaration-site warning. `c` is false, so the `and` short-
//!   circuits and `h.Make()` never runs in any of the 200 iterations; `h.Arg()` throws each
//!   time. Nothing was produced, so Made = 0 and N = 0 (a disposal of an unproduced receiver
//!   would run a destructor on garbage).
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
    var i := 0;
    var c := false;
    var h := new H();
    while i < 200 do
      try
        var b := c and h.Make().Boom() > 0;
        h.Arg();
      catch InvalidOperationException => i := i + 0;
      end try;
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made}");
  end Main;
end Program;
