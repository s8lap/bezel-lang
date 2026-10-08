//! case: s14_2_0027
//! doc: spec
//! section: 14.2
//! title: a class with a protected parameterless constructor does not satisfy where T: new()
//! rule: A class substitution satisfies `where T: new()` exactly when the
//!   class declares a parameterless constructor that is `public` — not
//!   merely accessible from some scope — (§14.2, v0.315). A class declaring
//!   only a `protected` parameterless constructor does not satisfy the
//!   constraint.
//! cites: 14.2
//! expect: reject
//! diagnostic: new()
module Conf;

// A class with only a protected parameterless constructor.
// It should NOT satisfy `where T: new()`.
public class Widget
  protected constructor()
  end constructor;
end Widget;

public static class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Main()
    var w := Factory.Make<Widget>();
  end Main;
end Program;
