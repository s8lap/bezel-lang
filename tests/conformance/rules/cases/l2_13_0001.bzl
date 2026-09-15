//! case: l2_13_0001
//! doc: stdlib
//! section: 2.13
//! title: Type.Name renders the unqualified declared identifier
//! rule: Name renders named components unqualified — for a named non-generic
//!   type, Name is the declared identifier alone.
//! cites: 2.13
//! expect: run
//! stdout: Widget
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var t := typeof(Widget);
    Console.WriteLine(t.Name);
  end Main;
end Program;
