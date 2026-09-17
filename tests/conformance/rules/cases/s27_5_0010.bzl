//! case: s27_5_0010
//! doc: spec
//! section: 27.5
//! title: using var on a function-typed local forbids reassignment
//! rule: A closure value may be bound to a local, plain `var` or `using var`
//!   alike, whose scope does not strictly enclose any source's scope, `using
//!   var` adding only non-reassignability (§28.3, v0.298).
//! cites: 27.5, 28.3, 28.10
//! expect: reject
//! diagnostic: reassignability
module Conf;

public class Program
  public method Demo()
    using var cb: () -> () := lambda() => Console.WriteLine("first");
    cb := lambda() => Console.WriteLine("second");
  end Demo;
end Program;
