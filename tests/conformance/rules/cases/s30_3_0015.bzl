//! case: s30_3_0015
//! doc: spec
//! section: 30.3
//! title: reading Result through ?. inside an async method is rejected
//! rule: The compiler rejects `Wait()` and `Result` lexically inside `async` methods and `async`
//!   lambdas
//! cites: 7.5
//! expect: reject
//! diagnostic: await
module Conf;

public class Program
  public static async method Main(): Task
    using var t := Task.FromResult(1);
    var t2: Task<int>? := t;
    var r := t2?.Result;
  end Main;
end Program;
