//! case: s28_3_8_3601
//! doc: spec
//! section: 28.3.8
//! title: a function-typed ref parameter's entry value stored into a sibling ref location is rejected
//! rule: A read carrying the unannounced source may be invoked, bound to a local or passed at a plain
//!   function-typed parameter, but not stored into a holder (the `ref` or `out` location itself and a
//!   sibling one included), passed at a set-erasing position or returned from the enclosing method.
//! cites: 28.3.8, 22.3, 28.14
//! expect: reject
//! diagnostic: unannounced
//! issue: 9093
//! notes: The non-generic function-value Swap. `t` holds `a`'s entry value; `a := b` stores `b`'s
//!   entry value into the sibling location. The admitted spelling is `Swap<T>(ref a: T, ref b: T)`.
module Conf;

public class Program
  public static method Swap(ref a: () -> int, ref b: () -> int)
    var t: () -> int := a;
    a := b;
    b := t;
  end Swap;
end Program;
