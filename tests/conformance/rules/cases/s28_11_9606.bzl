//! case: s28_11_9606
//! doc: spec
//! section: 28.11
//! title: a StringBuilder local stored into a field-held Dictionary is rejected
//! rule: the collection is a field, is reachable from one
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 9182
module Conf;

public class C
  private owned _d: Dictionary<string, StringBuilder> := new Dictionary<string, StringBuilder>();

  public method Put(k: string)
    var inner := new StringBuilder();
    inner.Append("x");
    _d[k] := inner;
  end Put;
end C;
