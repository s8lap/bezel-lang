//! case: s11_2_0108
//! doc: spec
//! section: 11.2
//! title: a non-generic candidate using the class type parameter does not match a generic member
//! rule: A candidate declaring a different number of type parameters of its own does not match.
//!   The spec's example: on a class C<T>, a non-generic Show(x: T): string whose T is the class's does not match a generic interface member Show<U>(x: U): string.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method Show<U>(x: U): string;
end I;

public class K<T>: I
  public constructor() end constructor;

  public method Show(x: T): string
    return "a";
  end Show;
end K;
