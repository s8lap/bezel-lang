//! case: s28_3_11_8518
//! doc: spec
//! section: 28.3.11
//! title: receiver temporaries in an iterator body are disposed once, on the normal edge and on a
//!   throw
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during
//!   unwinding by the same implicit `try`/`finally` mechanism as named locals
//! cites: 28.3.11, 29.1
//! expect: run
//! stdout: t=1575 N=51 Made=51
//! notes: Pins #9009. `Make()` returns an owning choice whose payload `Res` has a destructor;
//!   `Res.Made` counts constructions and `Res.N` destructor runs, so every produced receiver
//!   temporary must be disposed exactly once: N = Made. The static counters draw the
//!   expected §22.12 rule 2 declaration-site warning. The iterator yields `h.Make().Add(i)`
//!   = 7 + i for i = 0..49 (t = 350 + 1225 = 1575), producing 50 receivers disposed at each
//!   statement's boundary; at i = 3 a further `Make().Boom()` throws and its receiver is
//!   disposed by the unwind (the handler then swallows it): 51 produced, 51 disposed.
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
  public static method Gen(h: H): IEnumerable<int>
    var i := 0;
    while i < 50 do
      var s := h.Make().Add(i);
      yield return s;
      try var b := (i = 3) and h.Make().Boom() > 0; h.Arg(); catch InvalidOperationException => i := i + 0; end try;
      i := i + 1;
    end while;
  end Gen;
  public static method Main()
    var h := new H(); var t := 0;
    for var x in Gen(h) do t := t + x; end for;
    Console.WriteLine($"t={t} N={Res.N} Made={Res.Made}");
  end Main;
end Program;
