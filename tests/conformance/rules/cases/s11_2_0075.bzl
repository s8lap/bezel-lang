//! case: s11_2_0075
//! doc: spec
//! section: 11.2
//! title: a Dictionary parameter whose own parameters are swapped by position does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<K2, V2>(a: Dictionary<K2, Box<V2>>) corresponds by position to P<A, B>(a: Dictionary<B, Box<A>>) only if the key is the first own parameter; here the key is B, so the implementation's parameters are swapped.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<K2, V2>(a: Dictionary<K2, Box<V2>>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Dictionary<B, Box<A>>): int
    return 1;
  end P;
end K;
