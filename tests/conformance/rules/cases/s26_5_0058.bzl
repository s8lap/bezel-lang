//! case: s26_5_0058
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on the Task of a TaskCompletionSource is rejected
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, a parameter-count difference
//!   at a function-typed slot, a method-reference formation that fails, or an
//!   inference that finds no binding — the call is a compile error with no
//!   candidates cited, because no overload was reachable.
//! cites: 26.5, 22.6, 30.3, 30.11
//! expect: reject
//! diagnostic: Bogus
module Conf;

import System.Threading;

public class Program
  public static method Main(): int
    using var s := new TaskCompletionSource<int>();
    s.Task.Bogus();
    return 0;
  end Main;
end Program;
