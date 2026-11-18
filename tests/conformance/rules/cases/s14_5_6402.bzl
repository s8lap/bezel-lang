//! case: s14_5_6402
//! doc: spec
//! section: 14.5
//! title: an extension method's K left free by a null-bodied lambda is completed
//!   from a nullable local target
//! rule: A type parameter still unbound is bound from the call's **target type**,
//!   where the call site has one, by unifying the method's declared return type
//!   — current bindings applied — against the target;
//! cites: 14.5
//! expect: run
//! stdout: True
module Conf;

extension Array<T>
  public method E<K>(key: (T) -> K?): K?
    return key(this[0]);
  end E;
end;

public class Program
  public static method Main()
    var xs: Array<int> := [5, 6];
    var r: string? := xs.E(lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
