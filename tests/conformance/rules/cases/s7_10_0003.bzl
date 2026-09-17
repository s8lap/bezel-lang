//! case: s7_10_0003
//! doc: spec
//! section: 7.10
//! title: a nullable value type does not satisfy where T: value
//! rule: `where T: value` restricts T to non-nullable value types. A
//!   nullable value type (e.g. `int?`) does not satisfy the constraint.
//! cites: 14.2
//! expect: reject
//! diagnostic: does not satisfy
module Conf;

public class Box<T> where T: value
  public constructor()
  end constructor;
end Box;

public class Program
  public static method Main()
    var b := new Box<int?>();
  end Main;
end Program;
