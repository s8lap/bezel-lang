//! case: s28_9_0003
//! doc: spec
//! section: 28.9
//! title: a closure over a receiver-sourced borrow may be returned since its source outlives the call
//! rule: A function value that does not outlive its captured borrow's
//!   source is admitted — the compiler tracks captured borrows separately
//!   from the function value's own runtime-managed lifetime.
//! cites: 28.9, 28.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Registry
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Make(): () -> ()
    return lambda() => _w.Ping();
  end Make;
end Registry;
