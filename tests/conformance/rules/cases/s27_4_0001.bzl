//! case: s27_4_0001
//! doc: spec
//! section: 27.4
//! title: a top-level lambda with no target type must specify parameter types
//! rule: In top-level contexts with no target type (`var f := lambda(x) => x *
//!   2;`), parameter types must be specified.
//! cites: 27.4
//! expect: reject
//! diagnostic: specified
module Conf;

public class Program
  public method Demo()
    var f := lambda(x) => x * 2;
  end Demo;
end Program;
