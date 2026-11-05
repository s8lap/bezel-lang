//! case: s26_4_1103
//! doc: spec
//! section: 26.4
//! title: an extension member may name Channel<T> and ChannelSender<T> at the block's sendable T
//! rule: by the parameters' constraints, which are the block's `where` clause and nothing else
//! cites: 14.2
//! expect: accept
//! issue: 5998
module Conf;
import System.Threading;

extension Channel<T> where T: sendable
  public method P(c: Channel<T>, s: ChannelSender<T>): int
    return 1;
  end P;
end;
