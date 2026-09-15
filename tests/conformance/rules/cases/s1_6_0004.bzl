//! case: s1_6_0004
//! doc: spec
//! section: 1.6
//! title: a constructor closed with its own introducing keyword is accepted
//! rule: Keyword-labelled constructs are labelled by the keyword that
//!   introduced the block: `end constructor;`.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;
