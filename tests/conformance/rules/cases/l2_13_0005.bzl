//! case: l2_13_0005
//! doc: stdlib
//! section: 2.13
//! title: BaseType of a class with no explicit base is the Object Type
//! rule: BaseType is null only for Object, interface types, value types, and
//!   function types; a class declared with no base clause still resolves a
//!   non-null BaseType, terminating the chain at Object.
//! cites: 2.13
//! expect: run
//! stdout: Object
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    Console.WriteLine(typeof(Widget).BaseType!.Name);
  end Main;
end Program;
