//! case: s14_5_4829
//! doc: spec
//! section: 14.5
//! title: an extension method's K is completed from the typed-local target like a static call
//! rule: A type parameter still unbound is bound from the call's **target type**, where the call
//!   site has one, by unifying the method's declared return type — current bindings applied —
//!   against the target
//! cites: 14.5, 5.1
//! expect: run
//! stdout: none
//! issue: 8626
module Conf;

record Box(Name: string, Age: int);

extension Array<T>
  public method E<K>(key: (T) -> K?): K?
    return key(this[0]);
  end E;
end extension;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35)];
    var r: string? := xs.E(lambda(v) => null);
    Console.WriteLine(r ?? "none");
  end Main;
end Program;
