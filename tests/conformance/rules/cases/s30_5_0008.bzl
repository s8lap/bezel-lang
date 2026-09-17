//! case: s30_5_0008
//! doc: spec
//! section: 30.5
//! title: a named using var is the unsupervised remedy for discarding a task
//! rule: The two legal patterns for "I don't want to await this task" are
//!   `Task.FireAndForget(call())` and a **named** `using var t := call();`
//!   — unsupervised.
//! cites: 30.5, 30.10
//! expect: accept
module Conf;

public class Program
  public async method HandleAsync(): Task
    using var t := Task.Delay(0);
  end HandleAsync;
end Program;
