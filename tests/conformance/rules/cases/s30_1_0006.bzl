//! case: s30_1_0006
//! doc: spec
//! section: 30.1
//! title: async is rejected on a constructor
//! rule: `async` may appear on instance methods, static methods, and
//!   lambdas. It may not appear on constructors, destructors, or
//!   property/indexer accessors.
//! cites: 30.1
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Widget
  public async constructor()
  end constructor;
end Widget;
