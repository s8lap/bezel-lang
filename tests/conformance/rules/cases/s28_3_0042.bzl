//! case: s28_3_0042
//! doc: spec
//! section: 28.3
//! title: a property getter returning a receiver-sourced borrow is admitted
//! rule: A getter may lend a receiver-sourced borrow; expose a factory as a method
//!   returning `owned T` instead for a fresh value.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  private _w: Widget;

  public constructor(w: Widget)
    _w := w;
  end constructor;

  public property Fresh: Widget => _w;
end Factory;
