//! case: s28_3_0039
//! doc: spec
//! section: 28.3
//! title: an instance method that creates a fresh value through a non-owned return is rejected
//! rule: An instance method that produces a fresh value must declare `owned`; a
//!   freshly-produced owned value returned through a non-owned return is rejected.
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Make(): Widget
    return new Widget();
  end Make;
end Factory;
