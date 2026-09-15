module Base;

public class Money
  private _cents: int;

  public constructor(cents: int)
    _cents := cents;
  end constructor;

  public property Cents: int => _cents;
end Money;
