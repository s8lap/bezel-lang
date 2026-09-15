//! case: s15_1_0001
//! doc: spec
//! section: 15.1
//! title: an inline array with a defaultable element type default-initializes to zero
//! rule: When the element type T is defaultable, an inline array declaration
//!   without an explicit initializer is filled with that element default per
//!   slot (§15.1).
//! cites: 15.1
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var counts: [10]int;
    Console.WriteLine(counts[3]);
  end Main;
end Program;
