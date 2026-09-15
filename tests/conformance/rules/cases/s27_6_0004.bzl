//! case: s27_6_0004
//! doc: spec
//! section: 27.6
//! title: a reference to an overloaded method with no target type is ambiguous
//! rule: When no target type is available (e.g. `var f := Math.Abs;` with no
//!   annotation), the reference is ambiguous and the compiler reports an
//!   error pointing to the candidate overloads.
//! cites: 27.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public method Demo()
    var f := Math.Abs;
  end Demo;
end Program;
