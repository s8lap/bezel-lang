//! case: s28_3_0012
//! doc: spec
//! section: 28.3
//! title: move on a throw operand is rejected as redundant
//! rule: The transfer marker is rejected as redundant on a `throw` operand: `throw x;`,
//!   never `throw move x;`.
//! expect: reject
//! diagnostic: throw
module Conf;

public class Program
  public method Demo()
    var ex := new InvalidOperationException("boom");
    throw move ex;
  end Demo;
end Program;
