//! case: s27_3_0910
//! doc: spec
//! section: 27.3
//! title: a constant lambda body 5 at an (int) -> ulong target is admitted across signedness
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... implicitly converts to any integer type whose range contains its value
//! cites: 5.1
//! expect: run
//! output: 5
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (int) -> ulong := lambda(x) => 5;
    Console.WriteLine(f(1));
  end Main;
end Program;
