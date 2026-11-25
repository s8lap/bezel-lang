//! case: s35_5_8930
//! doc: spec
//! section: 35.5
//! title: a nested owning match whose inner arm throws drops the outer and the inner payloads exactly once
//! rule: exception edges discharge implicitly
//! cites: 35.5, 28.3, 29.2
//! expect: run
//! output: N=335 Made=335 S=667 C=67
//! notes: Pins #9094 (nesting). For `i mod 3 = 0` the outer `Text(r)` arm evaluates an inner `case h.Pick(i
//!   + 1)` which is `Two` and throws in `a.Id() + h.Kaboom()` (C=67); both the outer payload and the
//!   inner pair must be disposed on unwinding. Made = 67*3 + 67*2 = 335 = N. S = 67*7 + 66*3 = 667.
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
public class H
  public constructor() end constructor;
  public method Make(): OChoice return OChoice.Text(new Res()); end Make;
  public method MakeTwo(): OChoice return OChoice.Two(new Res(), new Res()); end MakeTwo;
  public method MakeE(): OChoice return OChoice.Empty; end MakeE;
  public method Pick(i: int): OChoice
    if i mod 3 = 0 then return OChoice.Text(new Res()); end;
    if i mod 3 = 1 then return OChoice.Two(new Res(), new Res()); end;
    return OChoice.Empty;
  end Pick;
  public method Kaboom(): int throw new Exception("boom"); end Kaboom;
  public method KaboomB(): bool throw new Exception("boom"); end KaboomB;
  public static method Eat(owned x: Res): int return x.Id(); end Eat;
  public static method EatThrow(owned x: Res): int throw new Exception("eat"); end EatThrow;
  public static method Eat2(owned x: Res, k: int): int return x.Id() + k; end Eat2;
  public static method Add(a: int, b: int): int return a + b; end Add;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var s := 0;
    var c := 0;
    var i := 0;
    while i < 200 do
      try
        var v := (case h.Pick(i) of Text(r) => r.Id() + (case h.Pick(i + 1) of Text(q) => q.Id(), Two(a, b) => a.Id() + h.Kaboom(), Empty => 3), Two(x, y) => x.Id(), Empty => 3);
        s := s + v;
      catch
        Exception => do c := c + 1; end
      end try;
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s} C={c}");
    return 0;
  end Main;
end Program;
