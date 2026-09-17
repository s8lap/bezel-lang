//! case: s15_4_0001
//! doc: spec
//! section: 15.4
//! title: a collection literal with no determinable target type is a compile error
//! rule: A collection literal has no standalone type; it is well-typed only
//!   in a context that provides the target type. A collection literal in a
//!   position where no target type is determinable is a compile error
//!   (§15.4).
//! cites: 15.4
//! expect: reject
//! diagnostic: target type
module Conf;

public class Program
  public static method Main()
    var x := [1, 2, 3];
  end Main;
end Program;
