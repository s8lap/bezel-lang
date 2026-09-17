//! case: s22_7_0009
//! doc: spec
//! section: 22.7
//! title: a class declaring no instance constructor has an implicit public parameterless one
//! rule: A class that declares **no instance constructor** and no
//!   primary-constructor parameter list has an implicit parameterless
//!   constructor: `public`, no parameters, an empty body, chaining implicitly
//!   per §12.8 — and it satisfies `where T: new()` on the same terms a
//!   declared public parameterless one does (§14.2), which is what `new T()`
//!   reaches at such a substitution.
//! cites: 22.7, 14.2, 12.8
//! expect: run
//! stdout: made
//! issue: 4364
module Conf;

public class Widget
  public Name: string := "made";
end Widget;

public class Factory
  public static method Make<T>(): owned T where T: class, new()
    return new T();
  end Make;
end Factory;

public class Program
  public static method Main()
    using var w := Factory.Make<Widget>();
    Console.WriteLine(w.Name);
  end Main;
end Program;
