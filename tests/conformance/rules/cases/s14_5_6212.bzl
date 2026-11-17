//! case: s14_5_6212
//! doc: spec
//! section: 14.5
//! title: a full explicit list types an un-annotated lambda argument from
//!   the listed types
//! rule: When a full explicit list is supplied, no inference runs.
//! cites: 14.5
//! expect: run
//! stdout: 3
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    Console.WriteLine(Seq.Map<string, int>("ann", lambda(v) => v.Length));
  end Main;
end Program;
