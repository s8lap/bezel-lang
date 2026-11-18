module Lib;
public class G
  public static method ShowFS<T>(f: () -> T, g: () -> T): string => $"{f()}{g()}";
end G;
