//! case: s22_10_0022
//! doc: spec
//! section: 22.10
//! title: an explicit-constructor middle class with a this-hop constructor stores the root's positional field
//! rule: "Each must chain to another constructor via `this(...)`; only the primary may chain to `base(...)`" ([§22.10]). `E2`'s argument selects `B2`'s convenience constructor, which chains to the explicit constructor that makes the base call; the root `A`'s positional `m` is then stored in `Msg`, so the counter reads 1 after `D2` and 2 after `E2`.
//! cites: 22.8
//! expect: run
//! output: LOG b
//!   b! N=1
//!   LOG 7
//!   7! N=2
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
  public Msg: string := m;
end A;
public class B2: A
  public constructor(x: string)
    base(G.Log(x));
  end constructor;
  public constructor(n: int)
    this(n.ToString());
  end constructor;
end B2;
public class D2(m: string): B2(m)
end D2;
public class E2(n: int): B2(n)
end E2;
public class Program
  public static method Main(): int
    using var d: D2 := new D2("b");
    Console.WriteLine($"{d.Msg} N={G.N}");
    using var e: E2 := new E2(7);
    Console.WriteLine($"{e.Msg} N={G.N}");
    return 0;
  end Main;
end Program;
