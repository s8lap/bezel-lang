module Conf;
// Family B: a store whose receiver the structural check leaves unclassified (the result of a call). EXPECT: report ("receiver unclassified").
public class Item
  public constructor()
  end constructor;
end Item;
public class Index
  private owned _items: Array<Item>;
  public constructor()
    _items := new Array<Item>();
  end constructor;
  public method Items(): Array<Item> => _items;
  public method Register(item: Item)
    Items().Add(item);
  end Register;
end Index;
