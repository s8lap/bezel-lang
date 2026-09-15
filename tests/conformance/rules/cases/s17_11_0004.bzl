//! case: s17_11_0004
//! doc: spec
//! section: 17.11
//! title: default of a nullable reference type is null
//! rule: "Reference types: `null` if the location is nullable."
//! cites: 17.11
//! expect: run
//! stdout: True
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w: Widget? := default(Widget?);
    Console.WriteLine(w = null);
  end Main;
end Program;
