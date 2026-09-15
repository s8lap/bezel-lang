//! case: s11_3_0003
//! doc: spec
//! section: 11.3
//! title: a field and a property with the same name clash
//! rule: A non-overloadable member -- a field, property, or nested type --
//!   additionally clashes with any other member of the type that shares its
//!   name, regardless of kind.
//! cites: 11.3
//! expect: reject
//! diagnostic: clash
module Conf;

public class Account
  private Total: decimal := 0m;
  public property Total: decimal;

  public constructor()
  end constructor;
end Account;
