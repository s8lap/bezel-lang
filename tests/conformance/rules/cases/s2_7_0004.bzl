//! case: s2_7_0004
//! doc: spec
//! section: 2.7
//! title: a dotted prefix matching no import is an error at the leftmost
//!   segment
//! rule: Otherwise, match the longest dotted prefix of the chain against an
//!   imported module's full name. A prefix matching *no* import is an error
//!   at `A` (naming the unresolved segment).
//! cites: 2.7
//! expect: reject
//! diagnostic: Foo
module Conf;

public class Program
  public static method Main()
    var x: Foo.Bar;
  end Main;
end Program;
