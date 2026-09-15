//! case: l2_11_0004
//! doc: stdlib
//! section: 2.11
//! title: an owned Subscription return is accepted at the declaration
//! rule: Methods returning a Subscription annotate the return type as owned
//!   Subscription so that the caller's leak-prevention obligation is explicit.
//! cites: 4.3
//! expect: accept
module Conf;

public static class Registry
  public static method Register(): owned Subscription
    return CancellationToken.None.Register(lambda() => ());
  end Register;
end Registry;
