//! case: s11_1_7201
//! doc: spec
//! section: 11.1
//! title: a generic method over a two-instantiation interface reaches each
//!   instantiation's member
//! rule: each under its own instantiation's substitution
//! cites: 11.1, 11.2, 14.1
//! expect: run
//! output: IS 5 2
//!   SI 3 10
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

public class Program
  public static method Use<U, T>(i: IM<U, T>, a: U, b: T): string => i.Put(a, b);

  public static method Main()
    var t := new Impl();
    Console.WriteLine(Program.Use<int, string>(t, 4, "ss"));
    Console.WriteLine(Program.Use<string, int>(t, "sss", 9));
  end Main;
end Program;
