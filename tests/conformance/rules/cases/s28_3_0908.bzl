//! case: s28_3_0908
//! doc: spec
//! section: 28.3
//! title: a record method returning its plain Array parameter through an unmarked Array return is rejected
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7, 9.4
//! expect: reject
//! diagnostic: receiver
//! issue: 8170
module Conf;

public record Wrapper(V: int)
  public method Pick(a: Array<int>): Array<int> => a;
end Wrapper;
