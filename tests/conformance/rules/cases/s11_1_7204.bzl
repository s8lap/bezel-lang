//! case: s11_1_7204
//! doc: spec
//! section: 11.1
//! title: a generic method over a three-parameter interface at two
//!   instantiations reaches each member
//! rule: each under its own instantiation's substitution
//! cites: 11.1, 11.2, 14.1
//! expect: run
//! output: ISB
//!   SIB
module Conf;

public interface IM<T, U, V>
  method Put(a: T, b: U, c: V): string;
end IM;

public class Impl: IM<int, string, bool>, IM<string, int, bool>
  public constructor()
  end constructor;

  public method Put(a: int, b: string, c: bool): string => "ISB";

  public method Put(a: string, b: int, c: bool): string => "SIB";
end Impl;

public class Program
  public static method Use<V, T, U>(i: IM<V, T, U>, a: V, b: T, c: U): string => i.Put(a, b, c);

  public static method Main()
    var t := new Impl();
    Console.WriteLine(Program.Use<int, string, bool>(t, 4, "ss", true));
    Console.WriteLine(Program.Use<string, int, bool>(t, "sss", 9, false));
  end Main;
end Program;
