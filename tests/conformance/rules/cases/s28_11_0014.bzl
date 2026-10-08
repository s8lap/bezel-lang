//! case: s28_11_0014
//! doc: spec
//! section: 28.11
//! title: GetOrAdd with a parameter-sourced key on a field-held dictionary is admitted
//! rule: `GetOrAdd(key, factory)` (stdlib §4.8, §4.9) is such a store at every
//!   call, whether or not the key is present: of `key`, and, into a borrowing
//!   value slot, of the factory's result, whose sources are what invoking the
//!   factory on `key` carries
//! cites: 28.11, 28.3
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Cache
  private owned _d: Dictionary<Item, int>;

  public constructor()
    _d := new Dictionary<Item, int>();
  end constructor;

  public method Touch(k: Item): int
    return _d.GetOrAdd(k, lambda(kk) => 1);
  end Touch;
end Cache;
