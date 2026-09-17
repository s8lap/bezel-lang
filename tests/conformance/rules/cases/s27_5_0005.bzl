//! case: s27_5_0005
//! doc: spec
//! section: 27.5
//! title: a closure may not discard a captured owned binding
//! rule: For a captured owned binding, an explicit `Dispose()` or discard of it
//!   is a compile error — a closure cannot consume, reassign, or dispose a
//!   captured owned binding (§28.3).
//! cites: 27.5, 28.3, 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(owned w: Widget)
    var drop: () -> () := lambda() => do _ := w; end;
    drop();
  end Demo;
end Program;
