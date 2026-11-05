//! case: s28_3_0907
//! doc: spec
//! section: 28.3
//! title: an instance method returning its StringBuilder parameter through an unmarked return is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Holder
  public constructor()
  end constructor;

  public method Pick(sb: StringBuilder): StringBuilder => sb;
end Holder;
