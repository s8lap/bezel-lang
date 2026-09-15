//! case: s30_1_0002
//! doc: spec
//! section: 30.1
//! title: falling off the end of a Task-returning async method is equivalent to return
//! rule: Falling off the end of a `Task`-returning async method is equivalent
//!   to `return;`.
//! cites: 30.1
//! expect: accept
module Conf;

public class Worker
  public async method RunAsync(): Task
    await Task.Delay(0);
  end RunAsync;
end Worker;
