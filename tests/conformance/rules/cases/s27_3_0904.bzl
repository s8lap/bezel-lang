//! case: s27_3_0904
//! doc: spec
//! section: 27.3
//! title: a lambda body of type double at a (double) -> float target is rejected
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... | `float` | `double` |
//! cites: 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var g: (double) -> float := lambda(x) => x;
    Console.WriteLine(g(1.5));
  end Main;
end Program;
