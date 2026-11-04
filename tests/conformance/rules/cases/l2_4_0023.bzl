//! case: l2_4_0023
//! doc: stdlib
//! section: 2.4
//! title: Get's result is string?, so member access on it is rejected
//! rule: public static method Get(name: string): string?;
//! cites: 2.4, 7.4
//! expect: reject
//! diagnostic: requires narrowing to non-null
//! issue: 8096
//! notes: twin of l2_4_0031 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    var n := Environment.Get("BZ_H3_A").Length;
  end Main;
end Program;
