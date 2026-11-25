//! case: s35_5_8929
//! doc: spec
//! section: 35.5
//! title: an owning match used as a call argument disposes the selected arm's payload bindings at its exit
//! rule: falling off the arm still owning the value auto-drops it with the must-use warning naming the
//!   binding and the standard remedies
//! cites: 35.5, 28.3, 29.2
//! expect: run
//! output: N=201 Made=201 S=1605 C=0
//! notes: Pins #9094 (position independence). The `case` expression is the second argument of `H.Add`, a
//!   position other than a `var` initializer. S = 67*7 + 67*14 + 66*3 = 1605; N = Made = 201.
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
      s := H.Add(s, (case h.Pick(i) of Text(r) => r.Id(), Two(a, b) => a.Id() + b.Id(), Empty => 3));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s} C={c}");
    return 0;
  end Main;
end Program;
