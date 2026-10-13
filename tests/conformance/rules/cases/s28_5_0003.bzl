//! case: s28_5_0003
//! doc: spec
//! section: 28.5
//! title: an owned field of interface type is admitted and disposes via dynamic dispatch
//! rule: Only fields whose type is a **class type or an interface type** take
//!   the `owned` field modifier as a marking of their own. ... Interface-typed
//!   owned fields are admitted because runtime dispatch resolves disposal to
//!   the underlying class's `Dispose()`
//! cites: 28.1, 28.5
//! expect: accept
module Conf;

public interface IWidget
  method Ping();
end IWidget;

public class WidgetImpl: IWidget
  public constructor()
  end constructor;

  public method Ping() => Console.WriteLine("ping");
end WidgetImpl;

public class Holder
  private owned _w: IWidget;

  public constructor()
    _w := new WidgetImpl();
  end constructor;
end Holder;
