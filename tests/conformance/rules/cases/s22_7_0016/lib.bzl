module Lib;

public class Base
  static constructor()
    Console.WriteLine("base static");
  end constructor;

  public constructor()
    Console.WriteLine("base ctor");
  end constructor;
end Base;
