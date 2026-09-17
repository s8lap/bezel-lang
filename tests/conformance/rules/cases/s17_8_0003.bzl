//! case: s17_8_0003
//! doc: spec
//! section: 17.8
//! title: null-forgiving preserves ownership — a borrow stays a borrow
//! rule: "The null-forgiving operator preserves ownership: ... if `x` is
//!   borrowed, `x!` is borrowed with the same source. The operator is purely
//!   a type-system assertion (with a runtime null check); it does not move,
//!   copy, or borrow beyond what reading `x` already does."
//! cites: 17.8
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _w: Widget?;

  public constructor(w: Widget)
    _w := w;
  end constructor;

  public method Peek(): Widget => _w!;
end Holder;
