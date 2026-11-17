//! case: s14_5_6211
//! doc: spec
//! section: 14.5
//! title: an explicit type-argument list shorter than the method's parameter
//!   list is rejected
//! rule: A shorter or a longer list is a compile error; there are no partial
//!   lists.
//! cites: 14.5
//! expect: reject
//! diagnostic: partial lists
module Conf;

public static class Seq
  public static method Map<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Map;
end Seq;

public class Program
  public static method Main()
    Console.WriteLine(Seq.Map<string>("ann", lambda(v) => v.Length));
  end Main;
end Program;
