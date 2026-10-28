module Conf;
// Family B: the indexer-set store of a parameter borrow into a field-held borrowing Array. EXPECT: report.
public class Item
  public constructor()
  end constructor;
end Item;
public class Index
  private owned _items: Array<Item>;
  public constructor(first: Item)
    _items := new Array<Item>();
    _items.Add(first);
  end constructor;
  public method Replace(item: Item)
    _items[0] := item;
  end Replace;
end Index;
