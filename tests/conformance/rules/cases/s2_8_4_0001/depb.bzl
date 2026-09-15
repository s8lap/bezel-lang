module DepB;
import Common;

public static class CallerB
  public static method Ping(): int => Box<int>.Bump();
end CallerB;
