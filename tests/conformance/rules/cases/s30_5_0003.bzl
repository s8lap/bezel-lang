//! case: s30_5_0003
//! doc: spec
//! section: 30.5
//! title: using var is the canonical binding for a Pattern B await result
//! rule: The owned binding forms are `using var x := await ...` (the
//!   canonical form ...) and `var x := await ...`, which introduces an
//!   owned local of class type T under the standard discharge rules.
//! cites: 30.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public async method HandleAsync(): Task
    using var t := Task.FromOwnedResult<Widget>(new Widget());
    using var r := await t;
  end HandleAsync;
end Program;
