//! case: s27_3_0909
//! doc: spec
//! section: 27.3
//! title: a constant lambda body 255 at an (int) -> byte target is admitted by constant narrowing
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... implicitly converts to any integer type whose range contains its value
//! cites: 5.1
//! expect: run
//! output: 255
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (int) -> byte := lambda(x) => 255;
    Console.WriteLine(f(1));
  end Main;
end Program;
