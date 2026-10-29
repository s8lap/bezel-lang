//! case: s28_3_0277
//! doc: spec
//! section: 28.3
//! title: using var over a coalesce of a borrowed field and a new object is a rejected mixed meet
//! rule: When the operands are owned (or owned-nullable), `??` is a two-branch *expression* and its result ownership follows the same meet rule as an `if`/`else` join — see the producer table in §28.3: `owned ?? owned` is owned (the chosen value is discharged once), and an owned arm mixed with a borrowed arm is rejected.
//! cites: 28.10, 28.3, 7.7
//! expect: reject
//! diagnostic: mixed
module Conf;
public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;
public record Pt(X: int, Y: int);
public record RS(Name: string, N: int);
public record Wrap(R: Res);
public class Box<T>
  public owned V: T;
  public constructor(owned v: T) V := v; end constructor;
end Box;
public class H
  public static owned readonly SF: Res := new Res();
  public owned C: Res := new Res();
  public owned NC: Res? := new Res();
  public owned F: OC := OC.Text(new Res());
  public owned G: OC? := OC.Text(new Res());
  public Bor: Res? := null;
  public N: int := 7;
  public S: string := "s";
  public P: Pt := new Pt(1, 2);
  public RR: RS := new RS("a", 1);
  public property PN: int => N;
  public property PC: Res => C;
  public method Self(): H => this;
  public method Mk(): owned Res return new Res(); end Mk;
  public method CloneH(): owned H return new H(); end CloneH;
  public constructor() end constructor;
end H;
public class Make
  public static method MkH(): owned H return new H(); end MkH;
  public static method MkHN(): owned H? return new H(); end MkHN;
  public static method MkRes(): owned Res return new Res(); end MkRes;
end Make;
public class Program
  public static method Main(): int
    for var i in 0..<20 do
      do var h := new H(); using var x := h.NC ?? new Res(); end;
    end;
    Console.WriteLine($"{Res.Count}");
    return 0;
  end Main;
end Program;
