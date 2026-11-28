//! case: s28_3_8_3603
//! doc: spec
//! section: 28.3.8
//! title: a function-typed ref parameter's entry value stored into an out location is rejected
//! rule: A read carrying the unannounced source may be invoked, bound to a local or passed at a plain
//!   function-typed parameter, but not stored into a holder (the `ref` or `out` location itself and a
//!   sibling one included), passed at a set-erasing position or returned from the enclosing method.
//! cites: 28.3.8, 22.3
//! expect: reject
//! diagnostic: unannounced
//! issue: 9093
module Conf;

public class Program
  public static method GetFrom(out a: () -> int, ref b: () -> int)
    a := b;
  end GetFrom;
end Program;
