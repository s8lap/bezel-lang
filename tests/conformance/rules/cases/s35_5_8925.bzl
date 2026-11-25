//! case: s35_5_8925
//! doc: spec
//! section: 35.5
//! title: an expression-position owning match taking the payload-free arm disposes nothing and leaks nothing
//! rule: falling off the arm still owning the value auto-drops it with the must-use warning naming the
//!   binding and the standard remedies
//! cites: 35.5, 28.3, 29.2
//! expect: run
//! output: N=0 Made=0 S=600
//! notes: Pins #9094 (boundary). `MakeE()` yields `Empty`, which has no payload, so no `Res` is ever made
//!   (Made=0) and none is disposed (N=0); 200 iterations of 3 give S=600. Guards the over-drop
//!   direction: a fix that drops a payload binding on the payload-free arm would crash or
//!   double-dispose. Value-only; green on both arms.
module Conf;
public class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Res.Made := Res.Made + 1; end constructor;
  destructor Res.N := Res.N + 1; end destructor;
  public method Id(): int => 7;
end Res;
public choice OChoice Text(owned r: Res), Two(owned a: Res, owned b: Res), Empty;
end OChoice;
public class Sink
  private owned _f: Res? := null;
  public constructor() end constructor;
  public method Put(owned x: Res) _f := x; end Put;
  public method Has(): bool => _f <> null;
end Sink;
public class H
  public constructor() end constructor;
  public method Make(): OChoice return OChoice.Text(new Res()); end Make;
  public method MakeTwo(): OChoice return OChoice.Two(new Res(), new Res()); end MakeTwo;
  public method MakeE(): OChoice return OChoice.Empty; end MakeE;
  public method Kaboom(): int throw new Exception("boom"); end Kaboom;
  public static method Eat(owned x: Res): int return x.Id(); end Eat;
  public static method Give(owned x: Res): owned Res return x; end Give;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var s := 0;
    var i := 0;
    while i < 200 do
      var v := (case h.MakeE() of Text(r) => r.Id(), Two(a, b) => 2, Empty => 3);
      s := s + v;
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
