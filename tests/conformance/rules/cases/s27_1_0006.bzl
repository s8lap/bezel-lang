//! case: s27_1_0006
//! doc: spec
//! section: 27.1
//! title: a named-argument call through a function-typed value is rejected
//! rule: A call through a function-typed value admits no named argument
//!   (v0.295): the named form at an invocation whose callee is a function-typed
//!   value is a compile error naming this rule.
//! cites: 27.1, 22.5
//! expect: reject
//! diagnostic: named argument
module Conf;

public class Program
  public method Demo()
    var caller: (a: int, b: int) -> int := lambda(x: int, y: int) => x - y;
    var result := caller(b := 2, a := 100);
  end Demo;
end Program;
