//! case: s17_7_0003
//! doc: spec
//! section: 17.7
//! title: new is rejected on an inline array
//! rule: "`new [3]int(0, 1, 2)` is rejected by the type checker — inline
//!   arrays do not declare constructors, and the `new` syntax has no meaning
//!   on them."
//! cites: 17.7, 15.1
//! expect: reject
//! diagnostic: inline array
module Conf;

public class Program
  public static method Main()
    var a := new [3]int(0, 1, 2);
  end Main;
end Program;
