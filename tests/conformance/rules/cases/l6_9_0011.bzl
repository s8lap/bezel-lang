//! case: l6_9_0011
//! doc: stdlib
//! section: 6.9
//! title: a static readonly Channel<T> draws rule 4's confinement warning
//! rule: a static readonly Channel<T> draws rule 4's warning: the channel
//!   object is the single-consumer receiving end and must stay confined to
//!   one consuming domain.
//! cites: 6.9, 22.12
//! expect: warn
//! diagnostic: confine
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public static class Registry
  private static readonly _inbox: Channel<Job> := new Channel<Job>();
end Registry;
