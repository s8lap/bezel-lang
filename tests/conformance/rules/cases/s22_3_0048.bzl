//! case: s22_3_0048
//! doc: spec
//! section: 22.3
//! title: a ref parameter of an owning inline array is a compile error at the declaration
//! rule: A `ref` parameter whose declared type is a move-only value type (15.3; an owning inline array [N owned]T among them) is a compile error at the declaration: no argument can bind it.
//! cites: 22.3, 15.3
//! expect: reject
//! diagnostic: move-only
//! issue: 5566
module Conf;

public class Conn
  public constructor() end constructor;
end Conn;

public class Program
  public static method Take(ref slots: [2 owned]Conn): int
    return 1;
  end Take;
end Program;
