//! case: s8_2_0001
//! doc: spec
//! section: 8.2
//! title: GetType on a class instance answers a module-qualified Type.FullName
//! rule: `v.GetType()` returns the `Type` value for the runtime type of `v`;
//!   `Type.FullName` renders a named type qualified by its declaring module path.
//! cites: 8.2, 8.5
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
    Console.WriteLine(w.GetType().FullName);
  end Main;
end Program;
