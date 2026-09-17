//! case: s7_10_0005
//! doc: spec
//! section: 7.10
//! title: a nullable reference type does not satisfy where T: class
//! rule: `where T: class` restricts T to non-nullable reference types —
//!   classes, interfaces, `string`, function types.
//! cites: 14.2
//! expect: reject
//! diagnostic: non-nullable reference
module Conf;

public class Crate<T> where T: class
  public constructor()
  end constructor;
end Crate;

public class Program
  public static method Main()
    var c := new Crate<string?>();
  end Main;
end Program;
