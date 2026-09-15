//! case: s9_2_0006
//! doc: spec
//! section: 9.2
//! title: a non-owning inline array of admissible element type is an admissible component
//! rule: Admissible component types include a non-owning inline array [N]T or
//!   [N]T? whose element type is admissible, compared element-by-element.
//! expect: run
//! stdout: True
module Conf;

public record Triple
  public readonly property Values: [3]int;

  public constructor(values: [3]int)
    Values := values;
  end constructor;
end Triple;

public class Program
  public static method Main()
    var a := new Triple([1, 2, 3]);
    var b := new Triple([1, 2, 3]);
    Console.WriteLine(a = b);
  end Main;
end Program;
