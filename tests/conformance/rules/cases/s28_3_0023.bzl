//! case: s28_3_0023
//! doc: spec
//! section: 28.3
//! title: a using var local is not reassignable
//! rule: `using var` is a scope pin: non-reassignable.
//! expect: reject
//! diagnostic: reassign
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var w := new Widget();
    w := new Widget();
  end Demo;
end Program;
