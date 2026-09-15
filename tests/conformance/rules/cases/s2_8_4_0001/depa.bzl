module DepA;
import Common;

public static class CallerA
  public static method Ping(): int => Box<int>.Bump();
end CallerA;
