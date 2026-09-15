//! case: s21_9_0025
//! doc: spec
//! section: 21.9
//! title: awaiting a resultless Task as a statement draws no unused-result warning
//! rule: Method calls returning `()` and `await` expressions on `Task` (no result)
//!   are the common cases; no warning is emitted for these.
//! cites: 21.9, 30.1
//! expect: quiet
module Conf;

public class Waiter
  public async method Pause(): Task
    await Task.Delay(1);
  end Pause;
end Waiter;
