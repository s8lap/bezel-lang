module Lib;

public class Base
  public constructor()
  end constructor;

  public method Tag(): string => "base";
end Base;

public class Holder<T> where T: Base
  public constructor()
  end constructor;

  public method Describe(item: T): string => item.Tag();
end Holder;
