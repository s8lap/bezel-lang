//! case: s22_6_6217
//! doc: spec
//! section: 22.6
//! title: under a full explicit list a lambda returning the listed return
//!   type is accepted
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 22.6, 14.5
//! expect: run
//! stdout: cho!
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    var r: string := Seq.Map<string, string>("cho", lambda(v) => v + "!");
    Console.WriteLine(r);
  end Main;
end Program;
