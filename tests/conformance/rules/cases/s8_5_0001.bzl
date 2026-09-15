//! case: s8_5_0001
//! doc: spec
//! section: 8.5
//! title: the default ToString for a class renders the qualified type name
//! rule: The compiler-generated default `ToString` for classes returns the
//!   type's qualified name, the module-path-qualified rendering `Type.FullName`
//!   defines.
//! cites: 8.5, 8.2
//! expect: run
//! stdout: Conf.Widget
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    Console.WriteLine($"{w}");
  end Main;
end Program;
