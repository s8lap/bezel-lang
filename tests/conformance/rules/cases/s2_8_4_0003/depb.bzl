module DepB;
import Common;

public static class CallerB
  public static method Ping(): int => Counter.Bump();
end CallerB;
