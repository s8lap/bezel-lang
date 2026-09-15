//! case: s15_1_0002
//! doc: spec
//! section: 15.1
//! title: an inline array's explicit initializer fills its slots in order
//! rule: The initializer is a comma-separated list of N expressions in
//!   brackets (§15.1).
//! cites: 15.1
//! expect: run
//! stdout: 128
module Conf;

public class Program
  public static method Main()
    var rgb: [3]byte := [255, 128, 64];
    Console.WriteLine(rgb[1]);
  end Main;
end Program;
