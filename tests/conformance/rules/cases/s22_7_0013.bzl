//! case: s22_7_0013
//! doc: spec
//! section: 22.7
//! title: new C() reaches the implicit parameterless constructor directly
//! rule: A class that declares **no instance constructor** and no
//!   primary-constructor parameter list has an implicit parameterless
//!   constructor: `public`, no parameters, an empty body. It is what `new C()`
//!   reaches on such a class.
//! cites: 22.7
//! expect: run
//! stdout: made
//! issue: 4364
module Conf;

public class Widget
  public Name: string := "made";
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Name);
  end Main;
end Program;
