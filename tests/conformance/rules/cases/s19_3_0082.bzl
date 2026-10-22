//! case: s19_3_0082
//! doc: spec
//! section: 19.3
//! title: an Object binding in a generic class method over T = int? is rejected at the instantiation site
//! rule: "a type pattern with binding is rejected at the instantiation site whatever the static answer, as `as` is" (§19.3, v0.340, *Type patterns* paragraph): a value substitution (here `int?`) has no box, and a binding to a reference type would form one.
//! cites: 12.7, 3.6
//! expect: reject
//! diagnostic: reference-typed binding
module Conf;

public class Box<T>
  public method G(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
end Box;

public class Program
  public static method Main()
    var b := new Box<int?>();
    Console.WriteLine(b.G(42));
  end Main;
end Program;
