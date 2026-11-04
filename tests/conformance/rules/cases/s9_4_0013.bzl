//! case: s9_4_0013
//! doc: spec
//! section: 9.4
//! title: a record setter body whose store lands in the receiver's own storage is rejected
//! rule: a record may still declare a block-form property or indexer (a member
//!   outside §9.2's list merely suppresses auto-equality, §9.3) — including a
//!   setter in the mediating shape above, every store landing beyond a class-
//!   or interface-typed hop — but a *setter body* whose store lands in the
//!   receiver's own storage is rejected like any other member body.
//! cites: 23.3
//! expect: reject
//! diagnostic: receiver
//! notes: twin s9_4_0014 — the same block-form setter storing beyond a
//!   class-typed hop is legal.
module Conf;

public record Gauge
  public property Raw: int;

  public constructor(raw: int)
    Raw := raw;
  end constructor;

  public property Doubled: int
    get => Raw * 2;
    set => Raw := value / 2;
  end Doubled;
end Gauge;
