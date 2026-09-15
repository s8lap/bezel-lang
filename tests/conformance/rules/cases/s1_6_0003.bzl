//! case: s1_6_0003
//! doc: spec
//! section: 1.6
//! title: a constructor closed with the wrong introducing keyword is rejected
//! rule: Keyword-labelled constructs are labelled by the keyword that
//!   introduced the block (`end constructor;`, `end destructor;`); a label
//!   that does not match the construct's introducing keyword is a compile
//!   error.
//! cites: 1.6, C.1
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Widget
  public constructor()
  end destructor;
end Widget;
