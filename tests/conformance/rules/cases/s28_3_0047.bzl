//! case: s28_3_0047
//! doc: spec
//! section: 28.3
//! title: an uninitialized local adopts the ownership state of its first assignment
//! rule: A local holding no classified value adopts the classification of the first
//!   assignment reaching it on each path; two owned-producing branches adopt owned
//!   on both.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(flag: bool)
    var w: Widget;
    if flag then
      w := new Widget();
    else
      w := new Widget();
    end;
    w.Dispose();
  end Demo;
end Program;
