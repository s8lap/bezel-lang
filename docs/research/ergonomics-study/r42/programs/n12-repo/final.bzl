module Data.Repo;

public interface IEntity<K>
  readonly property Id: K;
end IEntity;

// r42: `where T: IEntity<K>` rejects User: IEntity<int> at K = int ("does not satisfy the constraint `where T: IEntity`"; new)
// so the key type is fixed to int
public class Repo<T> where T: class, IEntity<int>
  private owned _items: OwningDictionary<int, T> := new OwningDictionary<int, T>();

  public property Count: int => _items.Count;

  public method Add(owned item: T)
    _items[item.Id] := item;
  end Add;

  public method FindOrNull(id: int): T? => _items.GetOrNull(id);

  public method Where(pred: (T) -> bool): owned Array<int>
    var ids := new Array<int>();
    for var k in _items.Keys do
      if pred(_items[k]) then ids.Add(k); end;
    end;
    return ids;
  end Where;

  public method Remove(id: int): bool => _items.Remove(id);
end Repo;

public class User(id: int, name: string, active: bool): IEntity<int>
  public readonly property Id: int := id;
  public readonly property Name: string := name;
  public readonly property Active: bool := active;
end User;

public class Program
  public static method Main()
    using var repo := new Repo<User>();
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
