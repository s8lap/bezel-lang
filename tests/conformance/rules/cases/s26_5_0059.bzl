//! case: s26_5_0059
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a generic Task<T> parameter is rejected
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, a parameter-count difference
//!   at a function-typed slot, a method-reference formation that fails, or an
//!   inference that finds no binding — the call is a compile error with no
//!   candidates cited, because no overload was reachable.
//! cites: 26.5, 22.6, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static method Poke<T>(t: Task<T>)
    t.Bogus();
  end Poke;
end Program;
