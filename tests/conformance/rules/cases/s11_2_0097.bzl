//! case: s11_2_0097
//! doc: spec
//! section: 11.2
//! title: an inline-array parameter whose own type parameters are in the same order by position matches
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   A member's own type parameters take part in the match by number and position, not by name. The interface's substitution maps each of the interface's type parameters to the implementing type's argument, and each of the interface member's own type parameters to the candidate's type parameter at the same position; parameter types are compared after that mapping.
//!   This is the accepting twin of the swapped form: the same two inline-array parameters, in the interface's order under the position correspondence.
//! cites: 11.2
//! expect: accept
//! issue: 6088
module Conf;

public interface I
  method P<T, U>(a: [3]T, b: [3]U): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: [3]A, b: [3]B): int
    return 1;
  end P;
end K;
