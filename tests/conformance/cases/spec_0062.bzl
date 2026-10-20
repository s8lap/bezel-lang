public class OwningCache<T> where T: class
  private owned _items: OwningArray<T>;

  public constructor() _items := new OwningArray<T>(); end;

  public method Add(owned item: T)
    _items.Add(move item);
  end Add;

  public method Drain(): owned IOwningEnumerable<T>
    return _items.Drain();
  end Drain;
end OwningCache;
