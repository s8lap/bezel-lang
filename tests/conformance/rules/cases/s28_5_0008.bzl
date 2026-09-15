//! case: s28_5_0008
//! doc: spec
//! section: 28.5
//! title: an owning-inline-array field needs no owned field modifier
//! rule: Inline-array fields with the `owned` slot marker (`[N owned]T`,
//!   `[N owned]T?`) are admissible as class fields without a separate `owned`
//!   modifier on the field; the inline array's own slot marker already signals
//!   the class's destructor must dispose each non-null slot.
//! cites: 28.1, 28.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Pool
  private slots: [4 owned]Widget?;
end Pool;
