//! case: s22_5_0006
//! doc: spec
//! section: 22.5
//! title: a default value may not reference instance state
//! rule: A default-value expression may not reference instance state
//!   (`this`, instance fields, instance properties) — the default belongs
//!   to the method's signature, not to a particular receiver's frame.
//! cites: 22.5
//! expect: reject
//! diagnostic: instance state
module Conf;

public class Widget
  private _base: int := 10;

  public constructor()
  end constructor;

  public method Compute(offset: int := _base): int => offset;
end Widget;
