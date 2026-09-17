//! case: s28_3_0040
//! doc: spec
//! section: 28.3
//! title: declaring owned on the return admits the freshly-produced value
//! rule: An instance method that creates something must say `owned`; the declared
//!   `owned` return admits the fresh value.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Make(): owned Widget
    return new Widget();
  end Make;
end Factory;
