//! case: l6_9_0002
//! doc: stdlib
//! section: 6.9
//! title: a sealed class with sendable fields admits Channel<T>
//! rule: T must be sendable; the instantiation is rejected otherwise
//!   (language spec §3.6).
//! cites: 6.9, 30.14
//! expect: accept
module Conf;

import System.Threading;

public sealed class Sendable
  public Value: int;
  public constructor(value: int) Value := value; end constructor;
end Sendable;

public class UseChannel
  public method Make()
    using var ch := new Channel<Sendable>();
  end Make;
end UseChannel;
