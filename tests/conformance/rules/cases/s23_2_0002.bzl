//! case: s23_2_0002
//! doc: spec
//! section: 23.2
//! title: an expression-bodied property without readonly is admitted
//! rule: An expression-bodied property's body is checked against the declared
//!   type; the shape expresses read-onlyness, so no `readonly` is written.
//! cites: 23.2
//! expect: accept
module Conf;

public class Rect
  public property Width: decimal;
  public property Height: decimal;

  public constructor(w: decimal, h: decimal)
    Width := w;
    Height := h;
  end constructor;

  public property Area: decimal => Width * Height;
end Rect;
