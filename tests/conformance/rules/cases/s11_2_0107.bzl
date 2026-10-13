//! case: s11_2_0107
//! doc: spec
//! section: 11.2
//! title: a candidate with more own type parameters than the interface member does not match
//! rule: A candidate declaring a different number of type parameters of its own does not match.
//!   The spec's example: Show<T, U>(x: T): string does not match an interface member Show<U>(x: U): string, since the candidate declares two own type parameters and the interface member one.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method Show<U>(x: U): string;
end I;

public class K: I
  public constructor() end constructor;

  public method Show<T, V>(x: T): string
    return "a";
  end Show;
end K;
