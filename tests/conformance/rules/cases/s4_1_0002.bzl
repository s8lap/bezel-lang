//! case: s4_1_0002
//! doc: spec
//! section: 4.1
//! title: an if condition of a non-bool type is a compile error
//! rule: Conditions of if/elif and while, and a case/catch arm's when guard,
//!   require bool.
//! cites: 4.1
//! expect: reject
//! diagnostic: bool
module Conf;

public class Checker
  public static method Run(n: int)
    if n then
    end if;
  end Run;
end Checker;
