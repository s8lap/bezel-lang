//! case: s28_3_9807
//! doc: spec
//! section: 28.3
//! title: A closure inside a generic class that takes its own owned parameter may store it into an owning slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: accept
//! notes: Pins #9196 (pick 0ca67dfe2): twin of s28_3_9806.
module Conf;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

public class Repo<K, T> where K: hashable where T: class
  private owned _items: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Go(key: K, owned item: T)
    var f := lambda(owned u: T): int => do _items[key] := u; return 0; end;
    _ := f(move item);
  end Go;
end Repo;
