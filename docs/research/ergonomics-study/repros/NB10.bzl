// bug: NB10
// title: false must-use warning on an owned parameter stored into a generic `OwningDictionary` slot
// issue: new (B2/B20 family)
// rests-on: spec §28.3: an owned-slot store is marker-free and discharges the binding; stdlib §4.9 OwningDictionary iteration (and so teardown) order is unspecified
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: quiet
// order: any
// stdout: 2
// stdout: close 1
// stdout: close 2
module P;
public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;
public class Repo<K, T> where K: hashable where T: class
  private owned _items: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Add(key: K, owned item: T)
    _items[key] := item;
  end Add;
  public property Count: int => _items.Count;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Add(1, new User(1));
    r.Add(2, new User(2));
    Console.WriteLine(r.Count);
  end Main;
end Program;
