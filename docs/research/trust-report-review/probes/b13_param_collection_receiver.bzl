module Conf;
// Family B: a store into a collection held by a PLAIN PARAMETER (caller storage) of a parameter-sourced borrow: admitted under the residual. EXPECT: report.
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Register(list: Array<Item>, item: Item)
    list.Add(item);
  end Register;
end Program;
