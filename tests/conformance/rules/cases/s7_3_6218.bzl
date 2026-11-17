//! case: s7_3_6218
//! doc: spec
//! section: 7.3
//! title: under a full explicit list a lambda returning null at a
//!   non-nullable listed return type is rejected
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 7.3, 22.6
//! expect: reject
//! diagnostic: nullable
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    var r: int := Seq.Map<string, int>("ann", lambda(v) => null);
    Console.WriteLine(r);
  end Main;
end Program;
