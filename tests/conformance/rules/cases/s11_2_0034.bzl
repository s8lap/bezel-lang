//! case: s11_2_0034
//! doc: spec
//! section: 11.2
//! title: a renamed bare function-typed return matches by position
//! rule: Return types must match exactly under the interface's substitution. Interface P<T, U>(): (T) -> U is matched by P<A, B>(): (A) -> B: the own type parameters correspond by position, so the names need not agree.
//! cites: 11.2
//! expect: accept
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): (T) -> U;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): (A) -> B
    throw new Exception("x");
  end P;
end K;
