//! case: s28_3_11_8803
//! doc: spec
//! section: 28.3.11
//! title: an unbound generic owning-choice receiver temporary is disposed when a later operand throws
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during unwinding by the same implicit `try`/`finally` mechanism as named locals
//! cites: 28.3.11, 28.3.10
//! expect: run
//! stdout: 800 800
//! notes: Pins #9106 (fix 4e5dbd3fe). Each `h.MakeW()` result is an unbound owned temporary (an owning choice
//!  holding a Res) used as a receiver. Per iteration two statements each produce two temporaries
//!  (`and`: Count is 1 so the right operand runs; `or`: Count < 0 is false so the right operand runs)
//!  and the right operand throws: 4 Res per iteration, 800 in all, each disposed once.
module Conf;
public class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Res.Made := Res.Made + 1; end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
public choice Wrap<T> Has(owned v: T), None;
  public method Count(): int => (case this of Has(v) => 1, None => 0);
  public method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
end Wrap;
public class H
  public constructor() end constructor;
  public method MakeW(): Wrap<Res> return Wrap<Res>.Has(new Res()); end MakeW;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    for var i in 0 ..< 200 do
      try
        var b := h.MakeW().Count() > 0 and h.MakeW().Boom() > 0;
      catch InvalidOperationException =>
        var c := 0;
      end try;
      try
        var b2 := h.MakeW().Count() < 0 or h.MakeW().Boom() > 0;
      catch InvalidOperationException =>
        var c := 0;
      end try;
    end for;
    Console.WriteLine($"{Res.N} {Res.Made}");
    return 0;
  end Main;
end Program;
