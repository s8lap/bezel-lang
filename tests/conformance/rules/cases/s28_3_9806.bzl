//! case: s28_3_9806
//! doc: spec
//! section: 28.3
//! title: A closure inside a generic class cannot store a captured owned parameter into an owning slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9196 (pick 0ca67dfe2): the same rule at a type-parameter element type. Twin: s28_3_9807.
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
    var f := lambda(): int => do _items[key] := item; return 0; end;
    _ := f();
  end Go;
end Repo;

public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Go(1, new User(1));
    Console.WriteLine("done");
  end Main;
end Program;
