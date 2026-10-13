//! case: s11_2_0086
//! doc: spec
//! section: 11.2
//! title: a substituted bare function return of a different return type does not match
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Under the substitution S := int, the interface's return is (int) -> int; the implementation's (int) -> string differs in the function's return type.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6092
module Conf;

public interface I<S>
  method P(): (S) -> S;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(): (int) -> string
    throw new Exception("x");
  end P;
end K;
