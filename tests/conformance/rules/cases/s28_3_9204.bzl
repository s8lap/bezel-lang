//! case: s28_3_9204
//! doc: spec
//! section: 28.3
//! title: a generic constructor reading an `owned` parameter after a dictionary-slot store is rejected
//! rule: **moved-from** — a later read is an error until reassigned
//! cites: 28.3.1
//! expect: reject
//! diagnostic: moved
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. The constructor spelling of 9201. Twin 9208 omits the
//!   read.
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
  public constructor(k: K, owned first: T)
    _a[k] := first;
    var y := first;
  end constructor;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>(1, new User(1));
  end Main;
end Program;
