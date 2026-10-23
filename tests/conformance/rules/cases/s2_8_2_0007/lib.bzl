module Lib;

public class Box<T>
  public class Item<U>
    public method Get(): int
      return 7;
    end Get;
  end Item;

  public static method Make(): owned Item<int>
    return new Item<int>();
  end Make;
end Box;
