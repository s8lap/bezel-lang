module Data.Repo;

public interface IEntity<K>
  readonly property Id: K;
end IEntity;

public class Repo<K, T> where K: hashable where T: class, IEntity<K>
  private owned _items: OwningDictionary<K, T> := new OwningDictionary<K, T>();

  public property Count: int => _items.Count;

  public method Add(owned item: T)
    _items[item.Id] := item;
  end Add;

  public method FindOrNull(id: K): T? => _items.GetOrNull(id);

  public method Where(pred: (T) -> bool): owned Array<K>
    var ids := new Array<K>();
    for var k in _items.Keys do
      if pred(_items[k]) then ids.Add(k); end;
    end;
    return ids;
  end Where;

  public method Remove(id: K): bool => _items.Remove(id);
end Repo;

public class User(id: int, name: string, active: bool): IEntity<int>
  public readonly property Id: int := id;
  public readonly property Name: string := name;
  public readonly property Active: bool := active;
end User;

public class Program
  public static method Main()
    using var repo := new Repo<int, User>();
    repo.Add(new User(1, "ann", true));
    repo.Add(new User(2, "bob", false));
    repo.Add(new User(3, "cho", true));
    Console.WriteLine($"find 2 -> {repo.FindOrNull(2)?.Name ?? "none"}");
    Console.WriteLine($"find 9 -> {repo.FindOrNull(9)?.Name ?? "none"}");
    using var active := repo.Where(lambda(u) => u.Active);
    Console.WriteLine($"active: {String.Join(", ", active.Select(lambda(i) => i.ToString()))}");
    Console.WriteLine($"removed 1: {repo.Remove(1)}");
    Console.WriteLine($"count {repo.Count}");
  end Main;
end Program;
