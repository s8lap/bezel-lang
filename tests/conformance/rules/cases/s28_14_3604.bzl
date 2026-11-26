//! case: s28_14_3604
//! doc: spec
//! section: 28.14
//! title: a += forward of a scoped parameter inside a locally-invoked lambda is rejected
//! rule: Callee-side uses inside a nested closure are escaping uses (*liftable*): invoking a scoped
//!   parameter through a locally-invoked closure is rejected in v1. The bound covers every callee-side use
//!   the closure body makes of the parameter — an invocation, a store into a `scoped` field, a forward to a
//!   `scoped` parameter — whether or not the closure escapes the callee
//! cites: 28.14, 17.2
//! expect: reject
//! diagnostic: scoped
//! issue: 9311
//! notes: Accepting twin s28_14_3607 makes the same forward in the method body itself.
module Conf;

public class Hub
  private owned readonly _changed: EventSource<double> := new EventSource<double>();

  public method Fire(t: double)
    _changed.Raise(t);
  end Fire;

  public method On(scoped handler: (double) -> ())
    var f := lambda() => do _changed += handler; end;
    f();
  end On;
end Hub;
