//! case: s17_13_0008
//! doc: spec
//! section: 17.13
//! title: a Task-family result may not be discarded with _ :=
//! rule: "**Exception: a `Task`-family result may not be discarded.** ...
//!   The discard is a compile error ... Use `Task.FireAndForget(expression)`
//!   for supervised fire-and-forget, or a named `using var t := expression;`."
//! cites: 17.13, 30.10
//! expect: reject
//! diagnostic: FireAndForget
module Conf;

public class Program
  public static method Main()
    _ := Task.Delay(10);
  end Main;
end Program;
