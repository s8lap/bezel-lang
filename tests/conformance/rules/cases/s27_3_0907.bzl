//! case: s27_3_0907
//! doc: spec
//! section: 27.3
//! title: a lambda body of type double at a (double) -> decimal target is rejected
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... Between `decimal` and binary floats (`float`, `double`).
//! cites: 5.1
//! expect: reject
//! diagnostic: never implicit
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var g: (double) -> decimal := lambda(x) => x;
    Console.WriteLine(g(2.5));
  end Main;
end Program;
