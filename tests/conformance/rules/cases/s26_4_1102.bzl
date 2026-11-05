//! case: s26_4_1102
//! doc: spec
//! section: 26.4
//! title: a generic extension on ChannelSender<T> with where T: sendable, notnull is admitted
//! rule: by the parameters' constraints, which are the block's `where` clause and nothing else
//! cites: 14.2
//! expect: accept
//! issue: 5998
module Conf;
import System.Threading;

extension ChannelSender<T> where T: sendable, notnull
  public method P(): int
    return 1;
  end P;
end;
