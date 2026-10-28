module Conf;
// Control (0131 §3 row): a purely local collection the parameter outlives. EXPECT: quiet.
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Use(xs: Array<Item>): int => xs.Count;
  public static method Register(item: Item): int
    using var tmp := new Array<Item>();
    tmp.Add(item);
    return Use(tmp);
  end Register;
end Program;
