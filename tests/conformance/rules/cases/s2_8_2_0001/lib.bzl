module Lib;

internal class Helper
  public static method Twice(n: int): int => n * 2;
end Helper;

public class Box<T>
  private _value: T;

  public constructor(value: T)
    _value := value;
  end constructor;

  public property Value: T => _value;
end Box;

public class Doubler
  public static method Wrap(n: int): owned Box<int> => new Box<int>(Helper.Twice(n));
end Doubler;
