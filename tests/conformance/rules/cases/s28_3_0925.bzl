//! case: s28_3_0925
//! doc: spec
//! section: 28.3
//! title: under the report mode, a method keeping its plain parameter is classified needs scoped
//! rule: At each field-held, collection-held and comparer-held line the warning also
//!   states the classification §28.15 gives the store: **bounded**, where that
//!   section admits the store as written; **needs `scoped`**, where a member that
//!   is not a constructor keeps a plain parameter, naming the parameter; or **no
//!   static form**, naming the reason — the store relates two parameters, an
//!   `Object` parameter is kept by a member that is not a constructor, or the
//!   holder is static storage or is reached through a borrow.
//! cites: 28.15
//! expect: warn
//! diagnostic: needs scoped
//! flags: --report-trust-admissions
//! issue: 8420
module Conf;

public class Owner
  public constructor()
  end constructor;
end Owner;

public class Pet
  private _owner: Owner? := null;

  public constructor()
  end constructor;

  public method Adopt(owner: Owner)
    _owner := owner;
  end Adopt;
end Pet;
