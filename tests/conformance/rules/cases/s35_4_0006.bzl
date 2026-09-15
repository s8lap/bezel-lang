//! case: s35_4_0006
//! doc: spec
//! section: 35.4
//! title: Task<T> declared over an owning choice is a compile error at the declaration
//! rule: No §30.5 producer row admits a move-only value result, so a Task<T> or
//!   OwningTask<T> declared over an owning choice is a compile error at the
//!   declaration.
//! cites: 35.4, 30.5
//! expect: reject
//! diagnostic: move-only
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice LoadOutcome
  Loaded(owned w: Widget),
  Failed;
end LoadOutcome;

public class Program
  public static async method Load(): Task<LoadOutcome>   // ERROR: Task<T> over a move-only choice
    return LoadOutcome.Failed;
  end Load;
end Program;
