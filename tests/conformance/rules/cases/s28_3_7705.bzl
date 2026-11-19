//! case: s28_3_7705
//! doc: spec
//! section: 28.3
//! title: a plain function-typed parameter passed to an Object parameter is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.14
//! expect: reject
//! diagnostic: set-erasing
module Conf;
public class Sink
  public constructor() end constructor;
  public method Take(o: Object): int return 1; end Take;
end Sink;
public class Program
  public static method Pass(s: Sink, q: () -> int): int
    return s.Take(q);
  end Pass;
end Program;
