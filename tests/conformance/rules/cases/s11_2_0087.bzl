//! case: s11_2_0087
//! doc: spec
//! section: 11.2
//! title: a renamed multi-parameter bare function-typed return matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Interface P<T, U>(): (T, U) -> T is matched by P<A, B>(): (A, B) -> A: each own parameter corresponds by position.
//! cites: 11.2
//! expect: accept
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): (T, U) -> T;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): (A, B) -> A
    throw new Exception("x");
  end P;
end K;
