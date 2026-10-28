module Conf;
// Control: a value-typed parameter stored into a field carries no borrow. EXPECT: quiet.
public class Counter
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
end Counter;
