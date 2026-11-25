//! case: s28_14_9302
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback returned directly through a conditional is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules, applied transitively)
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: call-bounded
//! notes: Harvested from the #9174 review (b_return_formal_cond). `return (if c then q else lambda() => 7)`
//!   returns the call-bounded parameter `q` itself on one arm, which the callee may not do ("returned").
//!   The other arm, a fresh capture-free lambda, is legal on its own. Deletion test: replacing `q` by
//!   `lambda() => 3` in the conditional compiles.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: () -> int, c: bool): () -> int
    return (if c then q else lambda() => 7);
  end Get;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    var f := fl.Get(lambda() => 5, true);
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
