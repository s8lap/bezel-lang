//! case: s28_3_0029
//! doc: spec
//! section: 28.3
//! title: returning a borrowed local with no source that outlives the call is rejected
//! rule: A borrowed local or parameter with no source that outlives the call may not be
//!   returned.
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Peek(): Widget
    var w := new Widget();
    return w;
  end Peek;
end Program;
