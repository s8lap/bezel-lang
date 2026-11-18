//! case: s14_5_6404
//! doc: spec
//! section: 14.5
//! title: a return against a declared nullable return type completes K left free by
//!   a null-bodied lambda
//! rule: A type parameter still unbound is bound from the call's **target type**,
//!   where the call site has one, by unifying the method's declared return type
//!   — current bindings applied — against the target;
//! cites: 14.5
//! expect: run
//! stdout: True
module Conf;

public static class Q
  public static method F<T, K>(x: T, key: (T) -> K?): K?
    return key(x);
  end F;
  public static method G(x: int): string?
    return Q.F(x, lambda(v) => null);
  end G;
end Q;

public class Program
  public static method Main()
    Console.WriteLine(Q.G(7) = null);
  end Main;
end Program;
