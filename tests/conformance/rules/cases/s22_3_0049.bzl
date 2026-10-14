//! case: s22_3_0049
//! doc: spec
//! section: 22.3
//! title: a ref parameter of a copyable inline array of the same class is admitted
//! rule: Move-only membership follows from structure: an inline array is move-only exactly when its slots carry the owned marker (15.3); without it the parameter is an ordinary ref parameter.
//! cites: 22.3, 15.3
//! expect: accept
//! issue: 5566
module Conf;

public class Conn
  public constructor() end constructor;
end Conn;

public class Program
  public static method Take(ref slots: [2]Conn): int
    return 1;
  end Take;
end Program;
