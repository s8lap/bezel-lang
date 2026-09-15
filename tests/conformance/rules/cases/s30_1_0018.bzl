//! case: s30_1_0018
//! doc: spec
//! section: 30.1
//! title: ensures is not admitted on an async method
//! rule: `requires` clauses on an `async` method evaluate in the synchronous
//!   prefix ...; `ensures` on `async` methods is not admitted in this
//!   revision (§22.16).
//! cites: 30.1, 22.16
//! expect: reject
//! diagnostic: ensures
module Conf;

public class Widget
  public async method LoadAsync(): Task<int>
    ensures(result >= 0)

    await Task.Delay(0);
    return 1;
  end LoadAsync;
end Widget;
