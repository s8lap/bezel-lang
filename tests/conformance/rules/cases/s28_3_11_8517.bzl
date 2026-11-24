//! case: s28_3_11_8517
//! doc: spec
//! section: 28.3.11
//! title: a receiver temporary live across a throw in an async method body is disposed once by
//!   the unwind
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during
//!   unwinding by the same implicit `try`/`finally` mechanism as named locals
//! cites: 28.3.11, 30.1
//! expect: run
//! stdout: r=50 N=50 Made=50
//! notes: Pins #9009. `Make()` returns an owning choice whose payload `Res` has a destructor;
//!   `Res.Made` counts constructions and `Res.N` destructor runs, so every produced receiver
//!   temporary must be disposed exactly once: N = Made. The static counters draw the
//!   expected §22.12 rule 2 declaration-site warning. The loop is in an `async` body with an
//!   `await` in each iteration before the throwing statement. Each of the 50 iterations
//!   produces one receiver, `Boom` throws, the unwind disposes it, the handler adds 1: r =
//!   50, Made = 50, N = 50.
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
  public async method Later(): Task<int> return 3; end Later;
end H;

class Program
  public static async method Run(h: H): Task<int>
    var i := 0; var k := 0;
    while i < 50 do
      try
        var w := await h.Later();
        k := k + h.Make().Boom() + w;
      catch InvalidOperationException => k := k + 1;
      end try;
      i := i + 1;
    end while;
    return k;
  end Run;
  public static async method Main(): Task<int>
    var h := new H();
    var r := await Run(h);
    Console.WriteLine($"r={r} N={Res.N} Made={Res.Made}");
    return 0;
  end Main;
end Program;
