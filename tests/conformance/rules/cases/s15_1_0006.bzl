//! case: s15_1_0006
//! doc: spec
//! section: 15.1
//! title: multi-dimensional inline arrays are not admitted
//! rule: Inline arrays may not be elements of inline arrays in the form
//!   `[N][M]T` — multi-dimensional inline arrays are deferred to a future
//!   revision (§15.1).
//! cites: 15.1
//! expect: reject
//! diagnostic: dimensional
module Conf;

public class Program
  public static method Foo()
    var m: [3][4]int;
  end Foo;
end Program;
