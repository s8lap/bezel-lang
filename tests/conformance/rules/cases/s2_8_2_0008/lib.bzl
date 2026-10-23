module Lib;

public class Holder
  public class Cell
    public method Get(): int
      return 5;
    end Get;
  end Cell;

  public static method Make(): owned Cell
    return new Cell();
  end Make;

  public static method Id(c: Cell): int
    return c.Get();
  end Id;
end Holder;
