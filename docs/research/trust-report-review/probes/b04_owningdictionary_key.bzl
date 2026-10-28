module Conf;
// Family B: the KEY slot of an OwningDictionary<K, V> is a borrowing position (§28.11 "the same check reaches the key slot"). EXPECT: report at the key.
public class Item
  public constructor()
  end constructor;
end Item;
public class Payload
  public constructor()
  end constructor;
end Payload;
public class Index
  private owned _by: OwningDictionary<Item, Payload>;
  public constructor()
    _by := new OwningDictionary<Item, Payload>();
  end constructor;
  public method Register(item: Item)
    _by.Add(item, new Payload());
  end Register;
end Index;
