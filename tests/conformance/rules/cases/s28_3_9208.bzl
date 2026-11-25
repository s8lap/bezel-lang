//! case: s28_3_9208
//! doc: spec
//! section: 28.3
//! title: a generic constructor storing its `owned` parameter into a dictionary slot consumes it
//! rule: store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor
//! cites: 28.3.1
//! expect: run
//! output: close 1
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. Admitting twin of 9204: the constructor stores `first`
//!   once and does not read it again: `close 1` at the holder disposal.
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
  end constructor;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>(1, new User(1));
  end Main;
end Program;
