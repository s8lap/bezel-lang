module Probe;
// CRITIQUE ROUND 2, NOTE 1, the forwarding loss: a class takes a comparer in its constructor and hands it to the dictionary it owns. Under 0212 revision 2 the parameter `cmp` is not static-storage-sourced, and `Dictionary<string, int>` is not a carrier type, so the construction is an error — whatever the caller passes, a `static readonly` instance included. The same body in a generic `Index<K>` is rejected where `Index<string>` is named and admitted at `Index<Item>` for a class `Item`. EXPECT today: the comparer-held trust clause admits it. RESULT on r40: not runnable — the comparer constructors are not implemented (as c06).
public class IgnoreCase: IEqualityComparer<string>
  public static readonly Instance: IgnoreCase := new IgnoreCase();
  public constructor()
  end constructor;
  public method Equals(a: string, b: string): bool => a.ToUpper() = b.ToUpper();
  public method GetHashCode(value: string): int => value.ToUpper().GetHashCode();
end IgnoreCase;
public class CaseIndex
  private owned _d: Dictionary<string, int>;
  public constructor(cmp: IEqualityComparer<string>)
    _d := new Dictionary<string, int>(cmp);
  end constructor;
  public method Put(key: string, value: int) => _d.Add(key, value);
  public method Has(key: string): bool => _d.ContainsKey(key);
end CaseIndex;
public class Program
  public static method Main(): int
    using var ix := new CaseIndex(IgnoreCase.Instance);
    ix.Put("Ann", 1);
    Console.WriteLine(ix.Has("ANN"));
    return 0;
  end Main;
end Program;
