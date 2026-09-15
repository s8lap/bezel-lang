//! case: s17_11_0006
//! doc: spec
//! section: 17.11
//! title: default of an inline array fills every slot with the element's zero value
//! rule: "Inline arrays whose element type is defaultable ... `default([3]int)`
//!   is `[0, 0, 0]`."
//! cites: 17.11
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a := default([3]int);
    var zeros: [3]int := [0, 0, 0];
    Console.WriteLine(a = zeros);
  end Main;
end Program;
