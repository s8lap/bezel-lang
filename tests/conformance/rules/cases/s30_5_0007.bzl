//! case: s30_5_0007
//! doc: spec
//! section: 30.5
//! title: Task.FireAndForget is the supervised remedy for discarding a task
//! rule: The two legal patterns for "I don't want to await this task" are
//!   `Task.FireAndForget(call())` (supervised: exceptions thrown by the
//!   task are logged) and a **named** `using var t := call();`.
//! cites: 30.5, 30.10
//! expect: accept
module Conf;

public class Program
  public async method HandleAsync(): Task
    Task.FireAndForget(Task.Delay(0));
    await Task.Delay(0);
  end HandleAsync;
end Program;
