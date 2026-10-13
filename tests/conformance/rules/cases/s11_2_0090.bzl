//! case: s11_2_0090
//! doc: spec
//! section: 11.2
//! title: a renamed Dictionary with a function-typed value in a bare return matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Interface P<T, U>(): Dictionary<T, (U) -> T> is matched by P<A, B>(): Dictionary<A, (B) -> A>: the own parameters correspond by position inside the Dictionary's key and function-typed value.
//! cites: 11.2
//! expect: accept
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): Dictionary<T, (U) -> T>;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): Dictionary<A, (B) -> A>
    throw new Exception("x");
  end P;
end K;
