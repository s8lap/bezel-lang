//! case: s28_3_0924
//! doc: spec
//! section: 28.3
//! title: under the report mode, a constructor's store of its plain parameter is classified bounded
//! rule: At each field-held, collection-held and comparer-held line the warning also
//!   states the classification §28.15 gives the store: **bounded**, where that
//!   section admits the store as written; **needs `scoped`**, where a member that
//!   is not a constructor keeps a plain parameter, naming the parameter; or **no
//!   static form**, naming the reason — the store relates two parameters, an
//!   `Object` parameter is kept by a member that is not a constructor, or the
//!   holder is static storage or is reached through a borrow. — §28.15 *The
//!   callee side* 1: into the object under construction, in a constructor
//! cites: 28.15
//! expect: warn
//! diagnostic: bounded
//! flags: --report-trust-admissions
//! issue: 8420
module Conf;

public class Owner
  public constructor()
  end constructor;
end Owner;

public class Pet
  private _owner: Owner;

  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
end Pet;
