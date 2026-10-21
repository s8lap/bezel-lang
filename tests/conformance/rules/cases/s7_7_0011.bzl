//! case: s7_7_0011
//! doc: spec
//! section: 7.7
//! title: a double literal is not implicitly convertible to an unconstrained type parameter in ??
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`". The implicit reference conversions name a type parameter only as the source, "a type parameter to an interface or class its constraints guarantee conformance to" ([§5.1]); a `double` value converts to an unconstrained `T` under no listed rule.
//! cites: 7.7, 5.1, 14.2
//! expect: reject
//! diagnostic: implicitly convert
//! issue: 6214
module Conf;

public class Box<T>
  public V: T;
  public constructor(x: T?)
    V := x ?? -1.0;
  end constructor;
end Box;

public class Program
  public static method Main()
    var b := new Box<double>(null);
    Console.WriteLine(b.V);
  end Main;
end Program;
