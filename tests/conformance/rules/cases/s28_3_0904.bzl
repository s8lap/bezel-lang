//! case: s28_3_0904
//! doc: spec
//! section: 28.3
//! title: an instance method returning the `!` of its nullable Array parameter is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Holder
  public constructor()
  end constructor;

  public method Pick(a: Array<int>?): Array<int> => a!;
end Holder;
