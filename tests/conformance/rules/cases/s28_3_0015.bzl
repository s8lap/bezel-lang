//! case: s28_3_0015
//! doc: spec
//! section: 28.3
//! title: a discard right-hand side carries no marker
//! rule: The discard itself is the announcement; a named owned binding discards
//!   unmarked.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    _ := w;
  end Demo;
end Program;
