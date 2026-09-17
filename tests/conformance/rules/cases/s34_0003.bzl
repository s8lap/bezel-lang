//! case: s34_0003
//! doc: spec
//! section: 34
//! title: a documentation comment not immediately before a declaration still compiles
//! rule: Documentation comments are meaningful only immediately before a
//!   declaration; the compiler preserves comments but does not interpret
//!   their content or restrict their placement.
//! cites: 1.5, 34
//! expect: accept
module Conf;

public class Widget
  public method Ping()
    /// this comment precedes a statement, not a declaration
    Console.WriteLine("ping");
  end Ping;
end Widget;
