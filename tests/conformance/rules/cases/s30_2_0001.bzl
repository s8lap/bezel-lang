//! case: s30_2_0001
//! doc: spec
//! section: 30.2
//! title: await on a value-typed Task<T> produces the result directly
//! rule: `await` suspends the current async method until a task completes.
//!   The shape `var result := await someTask;` is appropriate when
//!   `someTask` returns a value type, `string`, or `Task` (no value).
//! cites: 30.2
//! expect: accept
module Conf;

public class Counter
  public async method CountAsync(): Task<int>
    var n := await Task.FromResult(5);
    return n;
  end CountAsync;
end Counter;
