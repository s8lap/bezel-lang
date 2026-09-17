//! case: s15_3_0001
//! doc: spec
//! section: 15.3
//! title: reading a moved-from move-only local is a compile error
//! rule: A move-only value type is a value type whose instances cannot be
//!   duplicated; assignment transfers the value, leaving the source
//!   moved-from, and reading from a moved-from location is a compile error
//!   (§15.3).
//! cites: 15.3
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    var a: [2 owned]Connection := [new Connection(), new Connection()];
    var b := a;
    var c := a;
  end Main;
end Program;
