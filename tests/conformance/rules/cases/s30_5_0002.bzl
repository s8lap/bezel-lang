//! case: s30_5_0002
//! doc: spec
//! section: 30.5
//! title: a plain var binds the borrow produced by awaiting a Pattern A Task<T>
//! rule: The borrow produced by `await task` may flow to a `var` binding:
//!   `var r := await task;` — `r` is a non-owned local with the same source
//!   as `task`.
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
    using var task := Loader.LoadAsync();
    var r := await task;
  end HandleAsync;
end Program;
