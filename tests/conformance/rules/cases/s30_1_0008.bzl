//! case: s30_1_0008
//! doc: spec
//! section: 30.1
//! title: async is rejected on a property accessor
//! rule: `async` may appear on instance methods, static methods, and
//!   lambdas. It may not appear on constructors, destructors, or
//!   property/indexer accessors.
//! cites: 30.1
//! expect: reject
//! diagnostic: accessor
module Conf;

public class Holder
  public async property Ready: Task<bool> => Task.FromResult(true);
end Holder;
