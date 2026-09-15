//! case: s17_6_0004
//! doc: spec
//! section: 17.6
//! title: a read-only location cannot be incremented
//! rule: "The operand must be an assignable lvalue ... Read-only locations
//!   cannot be incremented."
//! cites: 17.6
//! expect: reject
//! diagnostic: const
module Conf;

public class Program
  public static method Main()
    const c := 5;
    c++;
  end Main;
end Program;
