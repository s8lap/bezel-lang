//! case: s28_3_9205
//! doc: spec
//! section: 28.3
//! title: a generic holder storing a borrowed parameter into an owning-dictionary slot is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an owning-collection slot, or an
//!   owned-state binding
//! cites: 28.3.1
//! expect: reject
//! diagnostic: owned
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. `item: T` is a borrow, `_a` an owning slot: the store is
//!   an error. Twin 9206 declares `owned item: T`.
module Conf;
public class User
  public Id: int;
  public constructor(id: int)
    Id := id;
  end constructor;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;
public class Repo<K, T> where K: hashable where T: class
  private owned _a: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Add(key: K, item: T)
    _a[key] := item;
  end Add;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    using var u := new User(1);
    r.Add(1, u);
  end Main;
end Program;
