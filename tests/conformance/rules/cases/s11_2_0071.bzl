//! case: s11_2_0071
//! doc: spec
//! section: 11.2
//! title: a class type parameter beside an own parameter swapped by position does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Under the substitution S to Z and the position correspondence U to W, the interface's second parameter is Box<W> and the implementation's is Box<Z>; the implementation swaps them.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P<U>(a: Box<S>, b: Box<U>): int;
end I;

public class K<Z>: I<Z>
  public constructor() end constructor;

  public method P<W>(a: Box<W>, b: Box<Z>): int
    return 1;
  end P;
end K;
