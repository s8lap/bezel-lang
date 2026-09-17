//! case: l6_9_0010
//! doc: stdlib
//! section: 6.9
//! title: a mutable static Channel<T> field is rejected
//! rule: A mutable static Channel<T> field is rejected by language spec
//!   §22.12 rule 2 (v0.181).
//! cites: 6.9, 22.12
//! expect: reject
//! diagnostic: mutable
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public static class Registry
  private static _inbox: Channel<Job> := new Channel<Job>();
end Registry;
