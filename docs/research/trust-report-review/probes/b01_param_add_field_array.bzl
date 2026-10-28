module Conf;
// Family B (s28_3_0093): parameter borrow Added to a field-held borrowing Array. EXPECT: report.
public class Item
  public constructor()
  end constructor;
end Item;
public class Index
  private owned _items: Array<Item>;
  public constructor()
    _items := new Array<Item>();
  end constructor;
  public method Register(item: Item)
    _items.Add(item);
  end Register;
end Index;
