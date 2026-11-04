//! case: s28_5_0023
//! doc: spec
//! section: 28.5
//! title: a fresh if-meet over new collections stored into a non-owned field is an error
//! rule: the field does not own its referent, so the fresh value would be left with no owner and leak
//! cites: 28.5, 28.3, 20
//! expect: reject
//! diagnostic: cannot be stored into the non-`owned` field
//! issue: 8060
module Conf;

public class Item
end Item;

public class Plain
  public p: OwningArray<Item>? := null;
end Plain;

public class Program
  public static method Run(i: int)
    var h := new Plain();
    h.p := (if i > 5 then new OwningArray<Item>() else new OwningArray<Item>());
  end Run;
end Program;
