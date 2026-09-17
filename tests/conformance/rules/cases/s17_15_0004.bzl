//! case: s17_15_0004
//! doc: spec
//! section: 17.15
//! title: a clause's own initializer does not see the binding it introduces
//! rule: "A binding is in scope from the end of its own clause ... its own
//!   initializer does not [see it]."
//! cites: 17.15
//! expect: reject
//! diagnostic: undeclared
module Conf;

public class Program
  public static method Main()
    var r := (var a := a + 1; a);
  end Main;
end Program;
