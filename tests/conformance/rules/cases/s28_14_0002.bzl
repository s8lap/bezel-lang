//! case: s28_14_0002
//! doc: spec
//! section: 28.14
//! title: storing a scoped callback into a non-scoped field is rejected
//! rule: The callee may store the callback only in scoped fields of the
//!   bounding object itself — a plain (non-scoped) field is not an
//!   admitted store target.
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
module Conf;

public class Registry
  private _cb: () -> ();

  public constructor(scoped cb: () -> ())
    _cb := cb;
  end constructor;
end Registry;
