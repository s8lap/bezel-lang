//! case: s28_3_9206
//! doc: spec
//! section: 28.3
//! title: a generic holder storing an `owned` parameter into a dictionary slot consumes it
//! rule: store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor
//! cites: 28.3.1
//! expect: run
//! output: 2
//!  close 1
//!  close 2
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. Admitting twin of 9201-9203 and 9205. Two adds, Count = 2;
//!   the holder disposes each User exactly once at scope end: `close 1`, `close 2`. Output-only (the
//!   corpus has no leak leg).
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
  public method Add(key: K, owned item: T)
    _a[key] := item;
  end Add;
  public property Count: int => _a.Count;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Add(1, new User(1));
    r.Add(2, new User(2));
    Console.WriteLine($"{r.Count}");
  end Main;
end Program;
