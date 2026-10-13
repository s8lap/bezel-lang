//! case: s11_2_0085
//! doc: spec
//! section: 11.2
//! title: a class type parameter substituted into a bare function-typed return matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Under the substitution S := int, interface I<int>'s member is P(): (int) -> int, and the implementation's return (int) -> int is exactly that type.
//! cites: 11.2
//! expect: accept
//! issue: 6092
module Conf;

public interface I<S>
  method P(): (S) -> S;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(): (int) -> int
    throw new Exception("x");
  end P;
end K;
