//! case: s13_2_0002
//! doc: spec
//! section: 13.2
//! title: internal is an admitted top-level visibility
//! rule: Top-level types may be `public` or `internal`.
//! cites: 13.2, C.2
//! expect: accept
module Conf;

internal class Helper
  public constructor()
  end constructor;
end Helper;
