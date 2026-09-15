//! case: s17_11_0012
//! doc: spec
//! section: 17.11
//! title: default of a nullable choice type is the valid null value
//! rule: "`default(C?)` is the valid null (for an owning `C`, move-only)
//!   value, as for owning inline arrays."
//! cites: 17.11, 35.10
//! expect: run
//! stdout: True
module Conf;

public choice Shape
  Circle(radius: double),
  Square(side: double);
end Shape;

public class Program
  public static method Main()
    var s := default(Shape?);
    Console.WriteLine(s = null);
  end Main;
end Program;
