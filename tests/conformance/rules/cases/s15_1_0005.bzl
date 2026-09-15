//! case: s15_1_0005
//! doc: spec
//! section: 15.1
//! title: inline arrays of different sizes are distinct types
//! rule: The size N is part of the type; `[3]int` and `[4]int` are distinct
//!   types that do not interconvert (§15.1).
//! cites: 15.1
//! expect: reject
//! diagnostic: distinct types
module Conf;

public class Program
  public static method Main()
    var a: [4]int := [1, 2, 3, 4];
    var b: [3]int := a;
  end Main;
end Program;
