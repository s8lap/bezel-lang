//! case: s30_5_0015
//! doc: spec
//! section: 30.5
//! title: a bare expression-statement Pattern B await is rejected
//! rule: For a `OwningTask<T>` not awaited in an owned-consuming context —
//!   a bare expression-statement `await t;`, whose minted result would be
//!   dropped silently — the await is a compile error.
//! cites: 30.5
//! expect: reject
//! diagnostic: owned-consuming
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public async method HandleAsync(): Task
    var t := Task.FromOwnedResult<Widget>(new Widget());
    await t;
  end HandleAsync;
end Program;
