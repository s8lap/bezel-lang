//! case: l9_3_0005
//! doc: stdlib
//! section: 9.3
//! title: a whole decimal renders as its integer digits
//! rule: stdlib §9.3 (v0.345) "Rendering and range": "`10m` renders `10`, and `0m` renders
//!   `0`" — the exact decimal form of the value, with no trailing-zero padding.
//! cites: 9.3
//! expect: run
//! stdout: 10
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(10m);
  end Main;
end Program;
