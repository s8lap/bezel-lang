module DepA;
import Common;

public static class CallerA
  public static method Ping(): int => Counter.Bump();
end CallerA;
