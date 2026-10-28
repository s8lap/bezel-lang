//! case: l9_3_0006
//! doc: stdlib
//! section: 9.3
//! title: zero renders as a single 0
//! rule: stdlib §9.3 (v0.345) "Rendering and range": "the integer digits (a single `0` when
//!   the magnitude is below one) … `0m` renders `0`".
//! cites: 9.3
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(0m);
  end Main;
end Program;
