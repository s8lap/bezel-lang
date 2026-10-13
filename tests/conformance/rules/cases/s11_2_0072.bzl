//! case: s11_2_0072
//! doc: spec
//! section: 11.2
//! title: an interface generic argument substituted with the wrong concrete type does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. The interface I<int> substitutes S by int, so its member is P(a: Box<int>); the implementation's Box<string> is not that type.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P(a: Box<S>): int;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(a: Box<string>): int
    return 1;
  end P;
end K;
