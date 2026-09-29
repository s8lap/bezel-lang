//! case: s30_5_0001
//! doc: spec
//! section: 30.5
//! title: using var may not bind the borrow produced by awaiting a Pattern A Task<T>
//! rule: The borrow may not flow to a `using var` binding: `using var r :=
//!   await task;` is rejected — a borrow cannot assume the disposal
//!   obligation the spelling would announce.
//! cites: 30.5
//! expect: reject
//! diagnostic: using var
//! issue: 4670
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Loader
  public static async method LoadAsync(): Task<Widget>
    await Task.Delay(0);
    return new Widget();
  end LoadAsync;
end Loader;

public class Program
  public async method HandleAsync(): Task
    using var task := Loader.LoadAsync();
    using var r := await task;
  end HandleAsync;
end Program;
