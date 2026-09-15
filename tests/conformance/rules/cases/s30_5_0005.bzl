//! case: s30_5_0005
//! doc: spec
//! section: 30.5
//! title: discarding a Task with the bare discard statement is rejected
//! rule: `_ := someTask` and `using var _ := someTask;` are **rejected** for
//!   the `Task` family — use `Task.FireAndForget(t)`, or a *named* `using
//!   var t := ...`.
//! cites: 30.5, 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public async method HandleAsync(): Task
    _ := Task.Delay(0);
  end HandleAsync;
end Program;
