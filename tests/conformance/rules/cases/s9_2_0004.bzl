//! case: s9_2_0004
//! doc: spec
//! section: 9.2
//! title: primitive, enum, string, and nullable components are all admissible
//! rule: Admissible component types include a primitive type, an enum, or string,
//!   and T? where T is admissible, compared by the null-safe = of section 8.4.
//! expect: run
//! stdout: True
module Conf;

public enum Color
  Red, Green, Blue;
end Color;

public record Paint
  public readonly property Name: string;
  public readonly property Shade: Color;
  public readonly property Note: string?;

  public constructor(name: string, shade: Color, note: string?)
    Name := name;
    Shade := shade;
    Note := note;
  end constructor;
end Paint;

public class Program
  public static method Main()
    var a := new Paint("wall", Color.Red, null);
    var b := new Paint("wall", Color.Red, null);
    Console.WriteLine(a = b);
  end Main;
end Program;
