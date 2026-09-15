//! case: s9_2_0001
//! doc: spec
//! section: 9.2
//! title: a body record with only auto-properties and a method is auto-hashable
//! rule: A body-only record receives auto-generated =, <>, GetHashCode, and
//!   ToString over its auto-properties when every instance member is an
//!   admissible auto-property, a method, a computed property, an eligible
//!   operator overload, or a constructor.
//! expect: run
//! stdout: True
module Conf;

public record Rectangle
  public readonly property Width: int;
  public readonly property Height: int;

  public constructor(width: int, height: int)
    Width := width;
    Height := height;
  end constructor;

  public method Area(): int => Width * Height;
end Rectangle;

public class Program
  public static method Main()
    var a := new Rectangle(2, 3);
    var b := new Rectangle(2, 3);
    Console.WriteLine(a = b);
  end Main;
end Program;
