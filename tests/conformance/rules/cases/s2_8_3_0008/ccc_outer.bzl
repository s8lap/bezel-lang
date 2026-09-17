module Outer;
import Mid;

public class Facade
  public static method Build(cents: int): owned Money => Wrapper.Make(cents);
end Facade;
