//! case: s9_2_0006
//! doc: spec
//! section: 9.2
//! title: a non-owning inline array of admissible element type is an admissible component
//! rule: Admissible component types include a non-owning inline array [N]T or
//!   [N]T? whose element type is admissible, compared element-by-element.
//! cites: 15.4
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
    var xs: [3]int := [1, 2, 3];
    var ys: [3]int := [1, 2, 3];
    var a := new Triple(xs);
    var b := new Triple(ys);
    Console.WriteLine(a = b);
  end Main;
end Program;
