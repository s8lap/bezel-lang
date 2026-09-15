//! case: s30_5_0005
//! doc: spec
//! section: 30.5
//! title: discarding a Task with the bare discard statement is rejected
//! rule: The only discards that remain errors are `Task`/`Task<T>` (use
//!   `Task.FireAndForget`, since disposing a task abandons a running
//!   operation — a semantic, not a leak, concern).
//! cites: 30.5, 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public async method HandleAsync(): Task
    _ := Task.Delay(0);
  end HandleAsync;
end Program;
