public class OwnedCache<T> where T: class
  private owned _items: OwningArray<T>;

  public constructor() _items := new(); end;

  public method Add(owned item: T)
    _items.Add(owned item);
  end Add;

  public method Drain(): IOwningEnumerable<T>
    return _items.Drain();
  end Drain;
end OwnedCache;
