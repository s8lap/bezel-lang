//! case: s30_10_0006
//! doc: spec
//! section: 30.10
//! title: the anonymous using-var binding of a Task is rejected
//! rule: The two legal patterns for "I don't want to await this task" are
//!   `Task.FireAndForget(call())` and a named `using var t := call();` — the
//!   anonymous `using var _ := call();` is rejected with the discard.
//! cites: 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static async method RunAsync(): Task
    await Task.Delay(0);
  end RunAsync;

  public static async method Main(): Task
    await Task.Delay(0);
    using var _ := RunAsync();
  end Main;
end Program;
