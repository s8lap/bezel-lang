//! case: s30_1_0011
//! doc: spec
//! section: 30.1
//! title: an owned parameter is admitted on an async method
//! rule: `owned` parameters transfer into the frame. An async method's
//!   `owned` parameters transfer into the state-machine frame at call time,
//!   exactly as an iterator's do.
//! cites: 30.1, 29.8
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Sink
  public async method ConsumeAsync(owned w: Widget): Task
    using var taken := move w;
    await Task.Delay(0);
  end ConsumeAsync;
end Sink;
