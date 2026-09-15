//! case: s30_5_0009
//! doc: spec
//! section: 30.5
//! title: the scoped await-bind admits a fresh var declaration over an unbound producer await
//! rule: In a local declaration `var x := await E;` ... where `E` is an
//!   unbound owned temporary of static type `Task<T>` for class T, no
//!   unbound temporary arises at all: the compiler binds it to a hidden
//!   scope-pinned slot, and `x` is an ordinary borrowed local.
//! cites: 30.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Loader
  public async method LoadAsync(): Task<Widget>
    await Task.Delay(0);
    return new Widget();
  end LoadAsync;
end Loader;

public class Program
  public async method HandleAsync(): Task
    var cfg := await Loader.LoadAsync();
    Use(cfg);
  end HandleAsync;

  private method Use(w: Widget)
  end Use;
end Program;
