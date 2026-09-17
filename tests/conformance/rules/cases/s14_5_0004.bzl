//! case: s14_5_0004
//! doc: spec
//! section: 14.5
//! title: conflicting bindings for one type parameter fail inference
//! rule: All bindings a type parameter receives must be the same type; two
//!   different bindings are an inference failure — the compiler computes no
//!   common supertype and no widened join (§14.5, *Agreement*).
//! cites: 14.5
//! expect: reject
//! diagnostic: bound to both
module Conf;

public static class Util
  public static method Same<T>(a: T, b: T): T => a;
end Util;

public class Program
  public static method Main()
    var x := Util.Same(1, 2L);
    Console.WriteLine(x);
  end Main;
end Program;
