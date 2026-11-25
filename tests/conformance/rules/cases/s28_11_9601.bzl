//! case: s28_11_9601
//! doc: spec
//! section: 28.11
//! title: a plain local builtin-collection borrow stored into a field-held Dictionary is rejected
//! rule: the collection is a field, is reachable from one
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 9182
module Conf;

public class C
  private owned _d: Dictionary<string, Dictionary<string, int>> := new Dictionary<string, Dictionary<string, int>>();

  public method Put(k: string)
    var inner := new Dictionary<string, int>();
    inner["x"] := 1;
    _d[k] := inner;
  end Put;
end C;
