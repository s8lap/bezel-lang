//! case: s30_11_0003
//! doc: spec
//! section: 30.11
//! title: returning the source's Task property out of its own scope is a borrow escape
//! rule: The task never outlives its source, and there is no first-read
//!   ownership transfer ... returning `tcs.Task` *itself* out of the scope
//!   is the return of a borrow sourced at a local with no source that
//!   outlives the call — rejected by §28.3's borrowed-return rule.
//! cites: 30.11, 28.3
//! expect: reject
//! diagnostic: borrow
module Conf;

import System.Threading;

public class Program
  public static method GetTask(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.TrySetResult(1);
    return tcs.Task;
  end GetTask;
end Program;
