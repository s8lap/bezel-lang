//! case: s7_4_0158
//! doc: spec
//! section: 7.4
//! title: invoking an un-narrowed nullable function value with a plain call is rejected
//! rule: Member access, method calls, and indexing on a nullable value are compile errors unless the
//!   compiler has narrowed it to non-null
//! cites: 7.6
//! expect: reject
//! diagnostic: narrowing
//! issue: 8099
module Conf;

public class Program
  public static method Main()
    var h: ((int) -> int)? := null;
    var r: int := h(1);
    Console.WriteLine(r);
  end Main;
end Program;
