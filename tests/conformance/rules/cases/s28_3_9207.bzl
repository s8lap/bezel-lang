//! case: s28_3_9207
//! doc: spec
//! section: 28.3
//! title: a generic holder storing `owned` parameters into an owning-array slot consumes them
//! rule: store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor
//! cites: 28.3.1
//! expect: run
//! output: close 1
//!  close 2
//! notes: Pins #9053 (fix 5488b0e1c): a store into an owning-collection slot whose element type is a type
//!   parameter consumes the `owned` parameter. `_items.Add(move first); _items[0] := second;` through
//!   `OwningArray<T>`: both Users disposed once by the holder (`close 1`, `close 2`).
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
public class Repo<T> where T: class
  private owned _items: OwningArray<T> := new OwningArray<T>(4);
  public method Put(owned first: T, owned second: T)
    _items.Add(move first);
    _items[0] := second;
  end Put;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<User>();
    r.Put(new User(1), new User(2));
  end Main;
end Program;
