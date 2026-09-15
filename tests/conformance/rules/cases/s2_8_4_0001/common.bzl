module Common;

public static class Box<T>
  public static Count: int := 0;

  public static method Bump(): int
    Count := Count + 1;
    return Count;
  end Bump;
end Box;
