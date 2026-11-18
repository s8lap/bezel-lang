//! case: s11_1_7202
//! doc: spec
//! section: 11.1
//! title: a generic class method over a two-instantiation interface with
//!   swapped parameters reaches each one
//! rule: each under its own instantiation's substitution
//! cites: 11.1, 11.2, 14.1
//! expect: run
//! output: SI 3 10
//!   IS 5 2
module Conf;

public interface IM<T, U>
  method Put(a: T, b: U): string;
end IM;

public class Impl: IM<int, string>, IM<string, int>
  public constructor()
  end constructor;

  public method Put(a: int, b: string): string => $"IS {a + 1} {b.Length}";

  public method Put(a: string, b: int): string => $"SI {a.Length} {b + 1}";
end Impl;

public class Holder<T, U>
  public constructor()
  end constructor;

  public method Use(i: IM<U, T>, a: U, b: T): string => i.Put(a, b);
end Holder;

public class Program
  public static method Main()
    var t := new Impl();
    using var h := new Holder<int, string>();
    Console.WriteLine(h.Use(t, "sss", 9));
    using var g := new Holder<string, int>();
    Console.WriteLine(g.Use(t, 4, "ss"));
  end Main;
end Program;
