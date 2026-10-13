//! case: s11_2_0118
//! doc: spec
//! section: 11.2
//! title: a return type substituted to int does not match an implementation returning string
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   Under I<int> the return type T is int; the implementation's return type string is not exactly equal to it.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I<T>
  method Get(): T;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method Get(): string
    return "hi";
  end Get;
end K;
