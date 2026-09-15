//! case: s4_2_0003
//! doc: spec
//! section: 4.2
//! title: the same literal at a declared long target is a target-typed integer constant
//! rule: an unsuffixed integer literal whose value exceeds int's range is
//!   admitted at a declared integer target whose range contains it, taking
//!   that target's type.
//! cites: 4.2
//! expect: run
//! stdout: 3000000000
module Conf;

public class Program
  public static method Main()
    var x: long := 3_000_000_000;
    Console.WriteLine($"{x}");
  end Main;
end Program;
