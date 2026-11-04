//! case: s9_8_0041
//! doc: spec
//! section: 9.8
//! title: a body record with an explicit non-skipped field is not serialization-eligible
//! rule: **Body records** (§9.2) are serialization-eligible under exactly the
//!   auto-equality membership conditions of §9.2 — the §9.2 member list and
//!   admissible-component rule are the single normative statement, applied
//!   here unchanged, evaluated with **both packages' skips applied**
//! cites: 9.8, 9.2
//! expect: reject
//! diagnostic: eligible
//! notes: twin s9_8_0042 — the same record with the field marked
//!   `equality skip`. §9.2 lists an explicit field declaration as a
//!   disqualifier.
module Conf;

public record Interval: ISerializable, IDeserializable
  public readonly property Lo: int;
  public readonly property Hi: int;
  private _cachedLen: int;

  public constructor(lo: int, hi: int)
    Lo := lo;
    Hi := hi;
    _cachedLen := hi - lo;
  end constructor;
end Interval;
