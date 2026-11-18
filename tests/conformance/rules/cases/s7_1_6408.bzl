//! case: s7_1_6408
//! doc: spec
//! section: 7.1
//! title: a nullable result completed through the nullable wrapping is not
//!   assignable to a non-nullable local
//! rule: `string` and `string?` are distinct types. The compiler enforces the
//!   distinction at every assignment, parameter pass, and return.
//! cites: 7.1, 14.5
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Holder<T>
  public method F<K>(x: T, key: (T) -> K?): K?
    return key(x);
  end F;
end Holder;

public class Program
  public static method Main()
    var h := new Holder<int>();
    var r: string := h.F(7, lambda(v) => null);
    Console.WriteLine(r = null);
  end Main;
end Program;
