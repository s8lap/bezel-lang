//! case: s28_11_9604
//! doc: spec
//! section: 28.11
//! title: an Array<int> local stored into a field-held Dictionary is rejected
//! rule: the collection is a field, is reachable from one
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 9182
module Conf;

public class C
  private owned _d: Dictionary<string, Array<int>> := new Dictionary<string, Array<int>>();

  public method Put(k: string)
    var inner := new Array<int>();
    inner.Add(1);
    _d[k] := inner;
  end Put;
end C;
