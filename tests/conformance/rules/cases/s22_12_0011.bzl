//! case: s22_12_0011
//! doc: spec
//! section: 22.12
//! title: a static readonly Channel field deliberately still warns
//! rule: A `static readonly Channel<T>` deliberately still warns
//!   (`Channel<T>` declares no facts) — the reminder that the channel is
//!   the single-consumer receiving end and must stay confined to one
//!   consuming domain.
//! cites: 22.12
//! expect: warn
//! diagnostic: single-consumer
module Conf;

public class Registry
  private static readonly _inbox: Channel<int> := new Channel<int>();
end Registry;
