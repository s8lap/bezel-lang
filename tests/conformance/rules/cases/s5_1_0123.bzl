//! case: s5_1_0123
//! doc: spec
//! section: 5.1
//! title: a negative hex literal that does not fit uint is rejected at the conversion site
//! rule: "uniformly with the target-typed constant's check against its declared target"
//! cites: 5.1, 4.2
//! expect: reject
//! diagnostic: does not fit the target's range
module Conf;

public class Program
  public static method Main()
    var u: uint := -0xB2D05E00;
    Console.WriteLine(u);
  end Main;
end Program;
