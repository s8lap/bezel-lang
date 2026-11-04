//! case: s21_1_0007
//! doc: spec
//! section: 21.1
//! title: a const initialized from an owned if-meet over new collections is rejected
//! rule: The initializer of a runtime `const` must not be an **owned production**
//! cites: 21.1, 28.3, 20
//! expect: reject
//! diagnostic: owned production
//! issue: 8060
module Conf;

public class Item
end Item;

public class Program
  public static method Run(i: int): int
    const u := (if i > 5 then new OwningArray<Item>() else new OwningArray<Item>());
    return u.Count;
  end Run;
end Program;
