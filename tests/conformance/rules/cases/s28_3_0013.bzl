//! case: s28_3_0013
//! doc: spec
//! section: 28.3
//! title: a throw operand carries no marker
//! rule: The `throw` keyword itself announces the consume; no marker is written at the
//!   operand.
//! expect: accept
module Conf;

public class Program
  public method Demo()
    var ex := new InvalidOperationException("boom");
    throw ex;
  end Demo;
end Program;
