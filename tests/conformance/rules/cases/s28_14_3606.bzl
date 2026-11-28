//! case: s28_14_3606
//! doc: spec
//! section: 28.14
//! title: invoking a scoped parameter through a locally-invoked closure is rejected
//! rule: Callee-side uses inside a nested closure are escaping uses (*liftable*): invoking a scoped
//!   parameter through a locally-invoked closure is rejected in v1. The bound covers every callee-side use
//!   the closure body makes of the parameter — an invocation, a store into a `scoped` field, a forward to a
//!   `scoped` parameter — whether or not the closure escapes the callee
//! cites: 28.14, 28.3.8
//! expect: reject
//! diagnostic: scoped
//! issue: 9311
module Conf;

public class Hub
  public method On(scoped handler: (double) -> ())
    var f := lambda(t: double) => handler(t);
    f(1.0);
  end On;
end Hub;
