//! case: s28_8_1_0022
//! doc: spec
//! section: 28.8.1
//! title: a using var as the replacement of a replacing move is rejected
//! rule: a borrow is rejected, as it is at any store into an owned location
//!   ... and so is a `using var`
//! cites: 28.8.1, 28.10
//! expect: reject
//! diagnostic: using var
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res();

  public method Install(): owned Res
    using var fresh := new Res();
    return move _r with fresh;
  end Install;
end Holder;
