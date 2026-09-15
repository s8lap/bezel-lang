//! case: s13_1_0003
//! doc: spec
//! section: 13.1
//! title: an internal member is not visible from another module
//! rule: `internal` — Visible to code in the same module. A module is the set
//!   of source files declaring the same module name presented to the module's
//!   one compilation; a separately-compiled importing module is not that set.
//! cites: 13.1, 13.4
//! expect: reject
//! diagnostic: internal
module App;
import Lib;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
