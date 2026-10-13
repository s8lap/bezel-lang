//! case: s11_2_0009
//! doc: spec
//! section: 11.2
//! title: a swapped pair of own type parameters inside a generic parameter type is rejected
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T, U>(a: Pair<T, U>) is not matched by P<A, B>(a: Pair<B, A>): the first slot is T (position 1) on the interface and B (position 2) on the implementation.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Pair<L, R>
  public constructor() end constructor;
end Pair;

public interface I
  method P<T, U>(a: Pair<T, U>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Pair<B, A>): int
    return 1;
  end P;
end K;
