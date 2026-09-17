module Common;

public static class Counter
  public static Value: int := 0;

  public static method Bump(): int
    Value := Value + 1;
    return Value;
  end Bump;
end Counter;
