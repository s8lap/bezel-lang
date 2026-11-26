//! case: s30_3_0003
//! doc: spec
//! section: 30.3
//! title: Result lexically inside an async method is rejected
//! rule: The compiler rejects `Wait()` and `Result` where the innermost enclosing method or lambda body
//!   is `async` (there, `await` is strictly superior and always available).
//! cites: 30.3
//! expect: reject
//! diagnostic: blocking wait
module Conf;

public class Widget
  public async method RunAsync(): Task
    using var t := Task.FromResult(1);
    var n := t.Result;
  end RunAsync;
end Widget;
