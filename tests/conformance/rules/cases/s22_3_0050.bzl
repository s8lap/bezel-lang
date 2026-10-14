//! case: s22_3_0050
//! doc: spec
//! section: 22.3
//! title: a ref parameter of a nullable owning inline array is a compile error at the declaration
//! rule: A `ref` parameter whose declared type is a move-only value type (15.3; the owning inline array [N owned]T? included) is a compile error at the declaration.
//! cites: 22.3, 15.3
//! expect: reject
//! diagnostic: move-only
//! issue: 5566
module Conf;

public class Conn
  public constructor() end constructor;
end Conn;

public class Program
  public static method Take(ref slots: [2 owned]Conn?): int
    return 1;
  end Take;
end Program;
