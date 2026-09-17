//! case: s28_5_0009
//! doc: spec
//! section: 28.5
//! title: double-marking an owning-inline-array field owned is rejected
//! rule: Writing `private owned slots: [16 owned]Connection?;` is rejected as a
//!   double-marking error — the field modifier is omitted because the
//!   type-level marker already conveys the obligation.
//! cites: 28.1, 28.5
//! expect: reject
//! diagnostic: double-marking
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Pool
  private owned slots: [4 owned]Widget?;
end Pool;
