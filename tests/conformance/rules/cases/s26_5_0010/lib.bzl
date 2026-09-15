module Lib;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

public extension Widget
  public method Doubled(): int => this.Size * 2;
end;
