//! case: s28_3_0909
//! doc: spec
//! section: 28.3
//! title: a generic class method returning its Array<T> parameter through an unmarked return is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7, 14.1
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Generic<T>
  public constructor()
  end constructor;

  public method Pick(a: Array<T>): Array<T> => a;
end Generic;
