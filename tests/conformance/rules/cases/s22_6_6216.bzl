//! case: s22_6_6216
//! doc: spec
//! section: 22.6
//! title: under a full explicit list a lambda returning a non-convertible
//!   type is rejected
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 22.6, 14.5
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    var r: string := Seq.Map<string, string>("ann", lambda(v) => v.Length);
    Console.WriteLine(r);
  end Main;
end Program;
