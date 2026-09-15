//! case: s30_10_0005
//! doc: spec
//! section: 30.10
//! title: discarding a Task with the discard statement is rejected
//! rule: `Task.FireAndForget` is the only supported way to discard a `Task`
//!   value — the discard form `_ := someAsyncCall();` is rejected at compile
//!   time because abandoning a `Task` reference would skip its continuation
//!   cleanup.
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
    _ := RunAsync();
  end Main;
end Program;
