//! case: s22_10_0021
//! doc: spec
//! section: 22.10
//! title: a primary-constructor root's positional field survives an explicit-constructor middle class
//! rule: "Parameters used only in field initializers or in the inheritance clause's base call are *not* captured: their values flow directly into the field they initialize or the base argument they supply" ([§22.10]). `D`'s `m` reaches `C`'s base call and `D`'s own initializer; `C`'s explicit constructor passes the logged string on to `A`, whose positional `m` the field `Msg` stores, so each construction reads `r1!` in `Msg`.
//! cites: 22.8
//! expect: run
//! output: LOG r1
//!   r1! r1!a r1d
//!   LOG r2
//!   r2! r2!a r2d
//!   LOG r3
//!   r3! r3!a r3d
//!   N=3
module Conf;
public static class G
  public static N: int := 0;
  public static method Log(x: string): string
    G.N := G.N + 1;
    Console.WriteLine($"LOG {x}");
    return x + "!";
  end Log;
end G;
public class A(m: string)
  public FA: string := m + "a";
  public Msg: string := m;
end A;
public class C: A
  public constructor(x: string)
    base(G.Log(x));
  end constructor;
end C;
public class D(m: string): C(m)
  public FD: string := m + "d";
end D;
public class Program
  public static method Main(): int
    var i: int := 0;
    while i < 3 do
      i := i + 1;
      using var d: D := new D("r" + i.ToString());
      Console.WriteLine($"{d.Msg} {d.FA} {d.FD}");
    end while;
    Console.WriteLine($"N={G.N}");
    return 0;
  end Main;
end Program;
