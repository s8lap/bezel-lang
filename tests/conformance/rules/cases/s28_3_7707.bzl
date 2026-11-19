//! case: s28_3_7707
//! doc: spec
//! section: 28.3
//! title: the result of invoking a plain function-typed parameter is rejected at an Object parameter
//! rule: a function value yields a value carrying the invoked value's set
//! cites: 28.3
//! expect: reject
//! diagnostic: set-erasing
module Conf;
public class Sink
  public constructor() end constructor;
  public method Take(o: Object): int return 1; end Take;
end Sink;
public class Program
  public static method Pass(s: Sink, q: () -> () -> int): int
    return s.Take(q());
  end Pass;
end Program;
