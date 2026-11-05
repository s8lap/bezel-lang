//! case: s28_3_0903
//! doc: spec
//! section: 28.3
//! title: an extension method returns a plain Array parameter that is not its receiver, which is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public class Host
  public constructor()
  end constructor;
end Host;

public extension Host
  public method Pick(a: Array<int>): Array<int> => a;
end extension;
