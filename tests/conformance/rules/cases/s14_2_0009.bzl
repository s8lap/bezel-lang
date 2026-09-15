//! case: s14_2_0009
//! doc: spec
//! section: 14.2
//! title: a class with a public parameterless constructor satisfies where T: new()
//! rule: For `where T: new()`, a class substitution satisfies the constraint
//!   exactly when the class declares a parameterless constructor that is
//!   `public` (§14.2).
//! cites: 14.2
//! expect: run
//! stdout: Conf.Widget
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public static class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Main()
    using var w := Factory.Make<Widget>();
    Console.WriteLine(w);
  end Main;
end Program;
