//! case: s28_6_0075
//! doc: spec
//! section: 28.6
//! title: disposing a task through a local alias is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow**
//!   is a compile error.
//! cites: 28.6, 28.2
//! expect: reject
//! diagnostic: is a borrow (an alias)
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t := Program.W();
    await t;
    var b := t;
    b.Dispose();
  end Main;
end Program;
