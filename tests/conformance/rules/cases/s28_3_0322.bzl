//! case: s28_3_0322
//! doc: spec
//! section: 28.3
//! title: a parenthesised fluent call over a field read returned under an owned return is rejected
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5, 28.3.4
//! expect: reject
//! diagnostic: borrow cannot be returned
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class H
  public sb: StringBuilder;
  public constructor(s: StringBuilder) this.sb := s; end constructor;
  public method Bad(): owned StringBuilder
    return (this.sb).Append("x");
  end Bad;
end H;
class C
  public static method Main(): int
    return 0;
  end Main;
end C;
