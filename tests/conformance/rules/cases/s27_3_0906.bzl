//! case: s27_3_0906
//! doc: spec
//! section: 27.3
//! title: a lambda body of type long at a (long) -> int target is rejected
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... | `long` | `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var g: (long) -> int := lambda(x) => x;
    Console.WriteLine(g(7L));
  end Main;
end Program;
