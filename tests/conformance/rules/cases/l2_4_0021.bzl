//! case: l2_4_0021
//! doc: stdlib
//! section: 2.4
//! title: Get's result is string?, so binding it to a string local is rejected
//! rule: The reverse requires a null check or `!`.
//! cites: 2.4, 7.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 8096
module Conf;

public class Program
  public static method Main()
    var x: string := Environment.Get("BZ_H3_A");
  end Main;
end Program;
