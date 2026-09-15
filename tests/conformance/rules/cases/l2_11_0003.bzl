//! case: l2_11_0003
//! doc: stdlib
//! section: 2.11
//! title: a plain (unmarked) Subscription return is rejected at the declaration
//! rule: Methods returning a Subscription annotate the return type as owned
//!   Subscription so that the caller's leak-prevention obligation is explicit.
//! cites: 4.3
//! expect: reject
//! diagnostic: owned
module Conf;

public static class Registry
  public static method Register(): Subscription
    return CancellationToken.None.Register(lambda() => ());
  end Register;
end Registry;
