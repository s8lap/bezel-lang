module Conf;
// Family B: AddRange of a parameter-held collection into a field-held borrowing collection — every element is parameter-sourced (value-copy member (ii)). EXPECT: report.
public class Item
  public constructor()
  end constructor;
end Item;
public class Index
  private owned _items: Array<Item>;
  public constructor()
    _items := new Array<Item>();
  end constructor;
  public method RegisterAll(items: Array<Item>)
    _items.AddRange(items);
  end RegisterAll;
end Index;
