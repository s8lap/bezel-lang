module Lib;

public static class Halver
  public static method Of(n: int): int
    requires(n >= 0)

    return n / 2;
  end Of;
end Halver;
