//! case: s30_1_0014
//! doc: spec
//! section: 30.1
//! title: a choice that holds borrows is rejected as an async parameter
//! rule: An `async` method ... may not declare a parameter whose type is a
//!   borrow-bearing record — or a choice that **holds borrows** (§35.4's
//!   holds-borrows property), whatever its classification.
//! cites: 30.1, 35.4
//! expect: reject
//! diagnostic: holds borrows
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Holder
  Some(w: Widget),
  None;
end Holder;

public class Program
  public async method HandleAsync(h: Holder): Task
    await Task.Delay(0);
  end HandleAsync;
end Program;
