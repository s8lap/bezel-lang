module Lib;
public class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; end constructor;
  public method Get(): (int)->int => this.Fn;
  public method GetB(): (int)->int
    return this.Fn;
  end GetB;
end Holder;
