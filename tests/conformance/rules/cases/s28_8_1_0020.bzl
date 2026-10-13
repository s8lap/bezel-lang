//! case: s28_8_1_0020
//! doc: spec
//! section: 28.8.1
//! title: a named owned binding is the replacement written unmarked
//! rule: The replacement `e` is an owned value for the location's declared
//!   type: a fresh producer, a `move` result, a named owned binding — the
//!   position is marker-free, as every store into an owned location is
//! cites: 28.8.1, 28.3
//! expect: accept
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res();

  public method Install(owned fresh: Res): owned Res
    return move _r with fresh;
  end Install;
end Holder;
