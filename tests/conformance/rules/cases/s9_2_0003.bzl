//! case: s9_2_0003
//! doc: spec
//! section: 9.2
//! title: computed properties are excluded from auto-equality and auto-ToString
//! rule: Computed (expression-bodied) properties are not part of the equality
//!   comparison or the ToString output even when they are visible on the public
//!   surface; equality and the auto-formatted representation are always over
//!   storage, never over derived values.
//! expect: run
//! stdout: True,Circle(Radius: 3)
module Conf;

public record Circle
  public readonly property Radius: int;
  public property Area: int => Radius * Radius;

  public constructor(radius: int)
    Radius := radius;
  end constructor;
end Circle;

public class Program
  public static method Main()
    var a := new Circle(3);
    var b := new Circle(3);
    Console.WriteLine($"{a = b},{a.ToString()}");
  end Main;
end Program;
