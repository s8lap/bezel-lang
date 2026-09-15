//! case: s30_1_0019
//! doc: spec
//! section: 30.1
//! title: requires on an async method is admitted and runs in the synchronous prefix
//! rule: `requires` clauses on an `async` method evaluate in the synchronous
//!   prefix — a violation surfaces at the call expression, on the caller's
//!   stack, before any suspension.
//! cites: 30.1, 22.16
//! expect: accept
module Conf;

public class Widget
  public async method LoadAsync(amount: int): Task<int>
    requires(amount > 0)

    await Task.Delay(0);
    return amount;
  end LoadAsync;
end Widget;
