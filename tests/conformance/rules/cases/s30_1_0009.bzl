//! case: s30_1_0009
//! doc: spec
//! section: 30.1
//! title: a ref parameter is rejected on an async method
//! rule: `ref` and `out` parameters are forbidden. An `async` method,
//!   `async` lambda, or iterator method may not declare a `ref` or `out`
//!   parameter; a compile error at the declaration.
//! cites: 30.1, 22.3
//! expect: reject
//! diagnostic: ref
module Conf;

public class Counter
  public async method BumpAsync(ref n: int): Task
    n := n + 1;
    await Task.Delay(0);
  end BumpAsync;
end Counter;
