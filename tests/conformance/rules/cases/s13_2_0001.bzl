//! case: s13_2_0001
//! doc: spec
//! section: 13.2
//! title: a top-level type declared protected is rejected
//! rule: Top-level types may be `public` or `internal`. They may not be
//!   `protected` (no enclosing class) or `private` (no file-scope concept).
//! cites: 13.2, C.2
//! expect: reject
//! diagnostic: public or internal
module Conf;

protected class Helper
  public constructor()
  end constructor;
end Helper;
