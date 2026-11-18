//! case: s14_5_6403
//! doc: spec
//! section: 14.5
//! title: an overload set completes K from a nullable local target and drops the
//!   inapplicable candidate
//! rule: A type parameter still unbound is bound from the call's **target type**,
//!   where the call site has one, by unifying the method's declared return type
//!   — current bindings applied — against the target;
//! cites: 14.5, 22.6
//! expect: run
//! stdout: True
module Conf;

public static class Q
  public static method F<T, K>(x: T, key: (T) -> K?): K?
    return key(x);
  end F;
  public static method F<T>(x: T, key: (T) -> int): int?
    return key(x);
  end F;
end Q;

public class Program
  public static method Main()
    var r: string? := Q.F(7, lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
