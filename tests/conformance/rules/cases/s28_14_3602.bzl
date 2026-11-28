//! case: s28_14_3602
//! doc: spec
//! section: 28.14
//! title: under the report mode, a scoped construction into an owned field of this reports its residual
//! rule: Where the storing object is instead held in an `owned` field of the bound … `move`-ing it out
//!   into longer-lived storage (`public method Take(): owned Child? => move _child;`) re-opens the window.
//!   That is *The registration-lifetime residual* above, not a new clause: a recorded trust clause that
//!   the trust-admission report names at the admitting site
//! cites: 28.14, 28.3
//! expect: warn
//! diagnostic: registration-lifetime
//! flags: --report-trust-admissions
//! issue: 8397
//! notes: The line is expected at the `_child := new Child(scoped this)` site. Contrast s28_3_0091,
//!   whose storing object is the bound itself and which reports nothing.
module Conf;

public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(scoped this);
  end constructor;
  public property Name: string => _name;
  public method Take(): owned Child? => move _child;
end Owner;

public class Child
  private _owner: Owner;
  public constructor(scoped owner: Owner)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Child;
