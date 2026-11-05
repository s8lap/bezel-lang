//! case: s28_3_0906
//! doc: spec
//! section: 28.3
//! title: an instance method returns an owned Array field reached through its parameter, which is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Source
  public owned Data: Array<int>;

  public constructor()
    Data := new Array<int>();
  end constructor;
end Source;

public class Holder
  public constructor()
  end constructor;

  public method Pick(s: Source): Array<int> => s.Data;
end Holder;
