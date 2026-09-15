//! case: s28_6_0025
//! doc: spec
//! section: 28.6
//! title: adding to a borrowed owning collection (pass-and-fill) is admitted
//! rule: Every operation but `Dispose` remains available on a borrowed owning
//!   collection — the idiomatic pass-and-fill pattern `method Fill(a:
//!   OwningArray<T>) a.Add(...)` takes a borrowed `OwningArray<T>` and adds
//!   to it.
//! cites: 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Fill(pool: OwningArray<Widget>)
    pool.Add(new Widget());
  end Fill;
end Program;
