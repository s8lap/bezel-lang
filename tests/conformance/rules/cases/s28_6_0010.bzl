//! case: s28_6_0010
//! doc: spec
//! section: 28.6
//! title: Dispose through a borrowed parameter is rejected
//! rule: `Dispose()` through a borrow is a compile error. The borrow
//!   receivers include a borrowed local or parameter.
//! cites: 28.6
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Stream
  public constructor()
  end constructor;
end Stream;

public class Program
  public method Close(s: Stream)
    s.Dispose();
  end Close;
end Program;
