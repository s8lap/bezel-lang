//! case: s11_6_0002
//! doc: spec
//! section: 11.6
//! title: a nested type missing its visibility keyword is rejected
//! rule: A nested type is a class member, and §13.3's explicit-visibility
//!   rule applies with no exception for nested types -- omitting the keyword
//!   is a compile error, not a default.
//! cites: 11.6, 13.3
//! expect: reject
//! diagnostic: visibility
module Conf;

public class Parser
  record Token(Kind: int, Text: string);

  public constructor()
  end constructor;
end Parser;
