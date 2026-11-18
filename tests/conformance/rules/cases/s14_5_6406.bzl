//! case: s14_5_6406
//! doc: spec
//! section: 14.5
//! title: an extension method whose K appears in no argument and no target context
//!   is a compile error
//! rule: A type parameter that remains unbound — it appears in no self-determined
//!   argument position and no target context supplies it — is a compile error
//!   naming the parameter
//! cites: 14.5
//! expect: reject
//! diagnostic: cannot infer
module Conf;

extension Array<T>
  public method E<K>(key: (T) -> K?): K?
    return key(this[0]);
  end E;
end;

public class Program
  public static method Main()
    var xs: Array<int> := [5, 6];
    var r := xs.E(lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
