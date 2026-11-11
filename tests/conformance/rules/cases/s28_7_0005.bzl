//! case: s28_7_0005
//! doc: spec
//! section: 28.7
//! title: a use of a local borrow of an owned field, dominated by a write to that field, is rejected
//! rule: "Use after a dominating field write." A local initialized from a
//!   direct read of an owned field — `var b := r.f;` — borrows the field's
//!   current value. A disposing write of the same path — a reassignment
//!   `r.f := e;` — that follows the borrow with no invalidating operation
//!   between them on any path from the borrow to the write. A use of the
//!   local — a read, a member access, an argument, a re-borrow — that such a
//!   write dominates on every path, the local a stable local (§7.4 *Stable
//!   locals*) not assigned between the borrow and the use, is a compile
//!   error, and so is a read of `r.f` itself after an explicit
//!   `r.f.Dispose()` or `r.f.DisposeAsync()` that dominates it in the same
//!   window; the diagnostic names the use, the borrow and the write.
//! cites: 28.7, 28.3
//! expect: reject
//! diagnostic: use after a dominating field write
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Replace()
    var b := _w;
    _w := new Widget();
    b.Ping();
  end Replace;
end Holder;
