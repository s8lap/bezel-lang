//! case: s14_5_6401
//! doc: spec
//! section: 14.5
//! title: a generic method's K left free by a null-bodied lambda is completed from a
//!   nullable local target
//! rule: A type parameter still unbound is bound from the call's **target type**,
//!   where the call site has one, by unifying the method's declared return type
//!   — current bindings applied — against the target;
//! cites: 14.5
//! expect: run
//! stdout: True
module Conf;

public class Holder<T>
  public method F<K>(x: T, key: (T) -> K?): K?
    return key(x);
  end F;
end Holder;

public class Program
  public static method Main()
    var h := new Holder<int>();
    var r: string? := h.F(7, lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
