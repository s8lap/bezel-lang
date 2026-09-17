//! case: s30_1_0010
//! doc: spec
//! section: 30.1
//! title: an out parameter is rejected on an async method
//! rule: `ref` and `out` parameters are forbidden. An `async` method,
//!   `async` lambda, or iterator method may not declare a `ref` or `out`
//!   parameter; a compile error at the declaration.
//! cites: 30.1, 22.3
//! expect: reject
//! diagnostic: out
module Conf;

public class Parser
  public async method TryParseAsync(out n: int): Task
    n := 0;
    await Task.Delay(0);
  end TryParseAsync;
end Parser;
