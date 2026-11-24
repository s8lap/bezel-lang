//! case: s28_3_11_8512
//! doc: spec
//! section: 28.3.11
//! title: an owning-choice receiver temporary is disposed once, at the end of the full expression
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3.11, 28.3
//! expect: run
//! stdout: s=1400 N=200 Made=200
//! notes: Pins #9009. `Make()` returns an owning choice whose payload `Res` has a destructor;
//!   `Res.Made` counts constructions and `Res.N` destructor runs, so every produced receiver
//!   temporary must be disposed exactly once: N = Made. The static counters draw the
//!   expected §22.12 rule 2 declaration-site warning. Normal edge, 200 iterations: Count() =
//!   7 each, s = 1400, Made = 200, N = 200.
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
    var i := 0; var s := 0; var h := new H();
    while i < 200 do s := s + h.Make().Count(); i := i + 1; end while;
    Console.WriteLine($"s={s} N={Res.N} Made={Res.Made}");
  end Main;
end Program;
