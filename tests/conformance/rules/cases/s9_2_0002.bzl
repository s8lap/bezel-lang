//! case: s9_2_0002
//! doc: spec
//! section: 9.2
//! title: auto-generated body-record ToString lists auto-properties in declaration order
//! rule: The generated ToString produces "TypeName(Prop1: value1, Prop2: value2)"
//!   over the auto-properties in declaration order.
//! expect: run
//! stdout: Rectangle(Width: 2, Height: 3)
module Conf;

public record Rectangle
  public readonly property Width: int;
  public readonly property Height: int;

  public constructor(width: int, height: int)
    Width := width;
    Height := height;
  end constructor;
end Rectangle;

public class Program
  public static method Main()
    var r := new Rectangle(2, 3);
    Console.WriteLine(r.ToString());
  end Main;
end Program;
