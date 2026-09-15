//! case: s22_7_0009
//! doc: spec
//! section: 22.7
//! title: a class declaring no instance constructor has an implicit public parameterless one
//! rule: A class that declares **no instance constructor** and no
//!   primary-constructor parameter list has an implicit parameterless
//!   constructor: `public`, no parameters, an empty body, chaining implicitly
//!   per §12.8. It is what `new C()` reaches on such a class.
//! cites: 22.7, 12.8, 14.2
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
