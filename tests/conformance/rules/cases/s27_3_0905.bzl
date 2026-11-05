//! case: s27_3_0905
//! doc: spec
//! section: 27.3
//! title: a lambda body of type int at an (int) -> ulong target is rejected
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... | `int` | `long`, `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var g: (int) -> ulong := lambda(x) => x;
    Console.WriteLine(g(7));
  end Main;
end Program;
