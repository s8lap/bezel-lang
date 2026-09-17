//! case: s14_2_0010
//! doc: spec
//! section: 14.2
//! title: a class without a public parameterless constructor fails where T: new()
//! rule: A class substitution satisfies `where T: new()` exactly when the
//!   class declares a parameterless constructor that is `public` — not
//!   merely accessible from some scope — or declares no instance constructor
//!   at all, in which case §22.7's implicit parameterless constructor, which
//!   is `public`, satisfies it on the same terms. A class declaring only a
//!   parameterized constructor satisfies neither arm (§14.2).
//! cites: 14.2, 22.7
//! expect: reject
//! diagnostic: new()
module Conf;

public class Widget
  public constructor(x: int)
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
