//! case: s23_2_0001
//! doc: spec
//! section: 23.2
//! title: readonly on an expression-bodied property is rejected
//! rule: On every body-bearing form read-onlyness is already expressed by the
//!   shape, and writing `readonly` there is a compile error.
//! cites: 23.2
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Rect
  public property Width: decimal;
  public property Height: decimal;

  public constructor(w: decimal, h: decimal)
    Width := w;
    Height := h;
  end constructor;

  public readonly property Area: decimal => Width * Height;
end Rect;
