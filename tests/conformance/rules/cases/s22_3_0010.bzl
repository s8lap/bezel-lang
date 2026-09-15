//! case: s22_3_0010
//! doc: spec
//! section: 22.3
//! title: ref is forbidden on an async method's parameters
//! rule: "`ref`/`out` are forbidden on `async` methods, on lambdas, on
//!   iterator methods and on indexers."
//! cites: 22.3
//! expect: reject
//! diagnostic: async
module Conf;

public class Worker
  public async method Process(ref n: int): Task
    n := n + 1;
    return;
  end Process;
end Worker;
