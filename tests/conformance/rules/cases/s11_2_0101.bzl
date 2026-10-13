//! case: s11_2_0101
//! doc: spec
//! section: 11.2
//! title: an inline array nested in a generic argument with the wrong substituted element does not match
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The interface's substitution maps S to int, so Box<[3]S> under I<int> is Box<[3]int>; the implementation's Box<[3]string> is not that type.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P(a: Box<[3]S>): int;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(a: Box<[3]string>): int
    return 1;
  end P;
end K;
