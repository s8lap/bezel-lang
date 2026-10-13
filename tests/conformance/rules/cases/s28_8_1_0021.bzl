//! case: s28_8_1_0021
//! doc: spec
//! section: 28.8.1
//! title: a borrow as the replacement of a replacing move is rejected
//! rule: a borrow is rejected, as it is at any store into an owned location
//! cites: 28.8.1, 28.6
//! expect: reject
//! diagnostic: borrow
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res();

  public method Install(fresh: Res): owned Res
    return move _r with fresh;
  end Install;
end Holder;
