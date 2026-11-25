//! case: s35_5_8926
//! doc: spec
//! section: 35.5
//! title: an expression-position owning match with declining guards drops only the selected arm's bindings
//! rule: ownership transfers into the selected arm's bindings **when an arm is selected**
//! cites: 35.5, 28.3, 29.2
//! expect: run
//! output: N=201 Made=201 S=1203 C=0
//! notes: Pins #9094. `Pick(i)` cycles Text (67x), Two (67x), Empty (66x). The guards `r.Id() > 100` and
//!   `a.Id() = 0` decline, so the selected arms are the unguarded `Text(q)`/`Two(x, y)`; ownership
//!   transfers only when an arm is selected, and the selected arm's bindings are dropped at the arm's
//!   exit. Made = 67 + 2*67 = 201 and every one is destroyed (N=201); S = 67*7 + 67*8 + 66*3 = 1203.
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
      var v := (case h.Pick(i) of Text(r) when r.Id() > 100 => 100, Text(q) => q.Id(), Two(a, b) when a.Id() = 0 => 0, Two(x, y) => x.Id() + 1, Empty => 3);
      s := s + v;
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s} C={c}");
    return 0;
  end Main;
end Program;
