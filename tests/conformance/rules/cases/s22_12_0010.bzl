//! case: s22_12_0010
//! doc: spec
//! section: 22.12
//! title: a static readonly field of a class outside the warning-suppression set warns
//! rule: A `static readonly` (including `owned`) field or auto-property
//!   of class type is admitted with a declaration-site warning unless the
//!   class is in this rule's warning-suppression set.
//! cites: 22.12
//! expect: warn
//! diagnostic: one domain
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Registry
  private static readonly _shared: Connection := new Connection();
end Registry;
