//! case: s17_11_0011
//! doc: spec
//! section: 17.11
//! title: default of a choice type is rejected
//! rule: "**Choices: no choice type is defaultable — `default(C)` is a
//!   compile error for every choice type** — a sum yields no distinguished
//!   zero case, and electing one would make program meaning depend on
//!   declaration order."
//! cites: 17.11, 35.10
//! expect: reject
//! diagnostic: defaultable
module Conf;

public choice Shape
  Circle(radius: double),
  Square(side: double);
end Shape;

public class Program
  public static method Main()
    var s := default(Shape);
  end Main;
end Program;
