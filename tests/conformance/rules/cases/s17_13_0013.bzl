//! case: s17_13_0013
//! doc: spec
//! section: 17.13
//! title: a Task?-typed result may not be discarded with _ := (nullable twin of 0008)
//! rule: "**The keying strips nullability** (v0.302), exactly as the
//!   [announcement table]'s Keying rule does for its own rows: a `Task?`,
//!   `Task<T>?` or `OwningTask<T>?` operand is a `Task`-family value here,
//!   and discarding it is the same compile error."
//! cites: 17.13, 28.3, 30.10
//! expect: reject
//! diagnostic: FireAndForget
module Conf;

public class Program
  public static method Main()
    var t: Task? := Task.Delay(10);
    _ := t;
  end Main;
end Program;