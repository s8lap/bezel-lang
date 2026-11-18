//! case: s14_5_6407
//! doc: spec
//! section: 14.5
//! title: an overload set whose K appears in no argument and no target context has
//!   no applicable candidate
//! rule: A type parameter that remains unbound — it appears in no self-determined
//!   argument position and no target context supplies it — is a compile error
//!   naming the parameter
//! cites: 14.5, 22.6
//! expect: reject
//! diagnostic: cannot infer
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
    var r := Q.F(7, lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
