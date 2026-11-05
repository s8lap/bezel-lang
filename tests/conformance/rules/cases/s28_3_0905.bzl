//! case: s28_3_0905
//! doc: spec
//! section: 28.3
//! title: an instance method returning an element read through its Array<Array<int>> parameter is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Holder
  public constructor()
  end constructor;

  public method Pick(a: Array<Array<int>>): Array<int> => a[0];
end Holder;
