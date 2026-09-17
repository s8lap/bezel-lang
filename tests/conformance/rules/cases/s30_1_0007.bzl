//! case: s30_1_0007
//! doc: spec
//! section: 30.1
//! title: async is rejected on a destructor
//! rule: `async` may appear on instance methods, static methods, and
//!   lambdas. It may not appear on constructors, destructors, or
//!   property/indexer accessors.
//! cites: 30.1
//! expect: reject
//! diagnostic: destructor
module Conf;

public class Widget
  public constructor()
  end constructor;

  async destructor
  end destructor;
end Widget;
