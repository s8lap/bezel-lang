//! case: s28_11_0036
//! doc: spec
//! section: 28.11
//! title: a bare move of an OwningDictionary entry is rejected and PopValue is the take that leaves nothing
//! rule: On an `OwningArray<T>` or `OwningDictionary<K, V>`, whose slots are
//!   `T` and `V` ... the replacement is required — `move xs[i] with fresh`,
//!   `move d[k] with fresh` ... "take the element and leave nothing" is
//!   `PopAt`/`PopValue` there.
//! cites: 28.11, 28.8.1
//! expect: reject
//! diagnostic: PopValue
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Program
  public method Demo()
    using var d := new OwningDictionary<string, Res>();
    d.Add("k", new Res());
    using var t := move d["k"];
  end Demo;
end Program;
