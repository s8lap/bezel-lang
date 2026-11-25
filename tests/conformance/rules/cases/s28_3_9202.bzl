//! case: s28_3_9202
//! doc: spec
//! section: 28.3
//! title: a generic holder storing an `owned` parameter into two dictionary slots is rejected
//! rule: **moved-from** — a later read is an error until reassigned
//! cites: 28.3.1
//! expect: reject
//! diagnostic: moved
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. The second store reads `item`, already moved-from by the
//!   first. Twin 9206 stores once.
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
  private owned _b: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Add(key: K, owned item: T)
    _a[key] := item;
    _b[key] := item;
  end Add;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Add(1, new User(1));
  end Main;
end Program;
