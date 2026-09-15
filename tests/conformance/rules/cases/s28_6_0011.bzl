//! case: s28_6_0011
//! doc: spec
//! section: 28.6
//! title: Dispose through an owned parameter is admitted
//! rule: Explicit disposal requires a receiver the disposer owns: an owned
//!   local or `owned` parameter, among the admitted receivers.
//! cites: 28.6
//! expect: accept
module Conf;

public class Stream
  public constructor()
  end constructor;
end Stream;

public class Program
  public method Close(owned s: Stream)
    s.Dispose();
  end Close;
end Program;
