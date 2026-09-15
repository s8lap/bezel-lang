//! case: s17_6_0003
//! doc: spec
//! section: 17.6
//! title: prefix ++ is a parse error — there is no prefix form
//! rule: "`++c` is a parse error. There is no prefix form."
//! cites: 17.6
//! expect: reject
//! diagnostic: prefix
module Conf;

public class Program
  public static method Main()
    var c := 5;
    ++c;
  end Main;
end Program;
