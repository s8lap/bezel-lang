//! case: l6_9_0001
//! doc: stdlib
//! section: 6.9
//! title: T must be sendable; a non-sendable class T is rejected
//! rule: T must be sendable; the instantiation is rejected otherwise
//!   (language spec §3.6).
//! cites: 6.9, 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public class NotSendable
  public Value: int;
  public constructor(value: int) Value := value; end constructor;
end NotSendable;

public class UseChannel
  public method Make()
    using var ch := new Channel<NotSendable>();
  end Make;
end UseChannel;
