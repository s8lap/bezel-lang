//! case: s14_5_6405
//! doc: spec
//! section: 14.5
//! title: a generic class method whose K appears in no argument and no target
//!   context is a compile error
//! rule: A type parameter that remains unbound — it appears in no self-determined
//!   argument position and no target context supplies it — is a compile error
//!   naming the parameter
//! cites: 14.5
//! expect: reject
//! diagnostic: cannot infer
module Conf;

public class Holder<T>
  public method F<K>(x: T, key: (T) -> K?): K?
    return key(x);
  end F;
end Holder;

public class Program
  public static method Main()
    var h := new Holder<int>();
    var r := h.F(7, lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
