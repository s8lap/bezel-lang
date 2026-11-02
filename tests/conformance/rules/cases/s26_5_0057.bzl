//! case: s26_5_0057
//! doc: spec
//! section: 26.5
//! title: an undeclared method awaited on a task in an async method is rejected
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
  public static async method Run(): Task<int>
    using var t := Task.FromResult(2);
    await t.Bogus();
    return 1;
  end Run;
end Program;
