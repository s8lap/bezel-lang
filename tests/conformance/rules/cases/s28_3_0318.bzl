//! case: s28_3_0318
//! doc: spec
//! section: 28.3
//! title: a property read returned under an owned return is rejected
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
  private _sb: StringBuilder;
  public constructor(s: StringBuilder) this._sb := s; end constructor;
  public property Sb: StringBuilder => this._sb;
  public method Bad(): owned StringBuilder
    return this.Sb;
  end Bad;
end H;
class C
  public static method Main(): int
    return 0;
  end Main;
end C;
