//! case: s7_3_6219
//! doc: spec
//! section: 7.3
//! title: under a full explicit list a lambda returning null at a nullable
//!   listed return type is accepted
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 7.3, 22.6
//! expect: run
//! stdout: null
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    var r: int? := Seq.Map<string, int?>("ann", lambda(v) => null);
    Console.WriteLine($"{r}");
  end Main;
end Program;
