//! case: s30_5_0016
//! doc: spec
//! section: 30.5
//! title: the explicit discard is an owned-consuming context for a Pattern B await
//! rule: The explicit discard `_ := await t;` is **not** such a case: the
//!   discard is itself an owned-consuming position and disposes the
//!   result.
//! cites: 30.5, 17.13
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public async method HandleAsync(): Task
    var t := Task.FromOwnedResult<Widget>(new Widget());
    _ := await t;
  end HandleAsync;
end Program;
