//! case: s2_7_0003
//! doc: spec
//! section: 2.7
//! title: chain head resolving to a type warns when the shadowed member's
//!   declared type differs
//! rule: Warning row — chain head resolves to a type while a same-named
//!   accessible member exists: warn, *unless* the member's declared type is
//!   the resolved type (or an instantiation of the resolved generic).
//!   `Status` the member is declared `string`, not `Status` the type, so the
//!   carve-out does not apply.
//! cites: 2.7
//! expect: warn
//! diagnostic: Status
module Conf;

public enum Status
  Active, Inactive;
end Status;

public class Widget
  public property Status: string;

  public constructor(s: string)
    Status := s;
  end constructor;

  public method Kind(): Status => Status.Active;
end Widget;
