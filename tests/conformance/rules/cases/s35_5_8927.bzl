//! case: s35_5_8927
//! doc: spec
//! section: 35.5
//! title: an expression-position owning match whose arm throws drops the unconsumed payloads on unwinding
//! rule: exception edges discharge implicitly
//! cites: 35.5, 28.3, 29.2
//! expect: run
//! output: N=201 Made=201 S=1136 C=67
//! notes: Pins #9094. In the `Text` arm `r.Id() + h.Kaboom()` throws for the 67 `Text` scrutinees (C=67);
//!   the exception edge must still dispose `r` (N=201 = Made=201). S = 67*0 (thrown) + 67*14 + 66*3 =
//!   1136.
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
        var v := (case h.Pick(i) of Text(r) => r.Id() + h.Kaboom(), Two(a, b) => a.Id() + b.Id(), Empty => 3);
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
