//! case: s30_3_0002
//! doc: spec
//! section: 30.3
//! title: Wait() lexically inside an async method is rejected
//! rule: The compiler rejects `Wait()` and `Result` lexically inside
//!   `async` methods and `async` lambdas (there, `await` is strictly
//!   superior and always available).
//! cites: 30.3
//! expect: reject
//! diagnostic: blocking wait
module Conf;

public class Widget
  public async method RunAsync(): Task
    using var t := Task.FromResult(1);
    t.Wait();
  end RunAsync;
end Widget;
