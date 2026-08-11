// base.bzl
module Base;

public class Money
  private _cents: long;

  public constructor(cents: long)
    _cents := cents;
  end constructor;

  public property Cents: long => _cents;

  public method Format(): string
    return $"{_cents} cents";
  end Format;
end Money;
