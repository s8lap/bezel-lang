//! case: s27_3_0908
//! doc: spec
//! section: 27.3
//! title: a constant lambda body 256 at an (int) -> byte target is rejected, outside byte's range
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... implicitly converts to any integer type whose range contains its value
//! cites: 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (int) -> byte := lambda(x) => 256;
    Console.WriteLine(f(1));
  end Main;
end Program;
