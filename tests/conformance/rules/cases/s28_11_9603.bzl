//! case: s28_11_9603
//! doc: spec
//! section: 28.11
//! title: a borrow stored through an alias of a scope-bound builtin local is rejected
//! rule: a borrow of a **scope-bound binding**
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
    var alias := inner;
    _d[k] := alias;
  end Put;
end C;
