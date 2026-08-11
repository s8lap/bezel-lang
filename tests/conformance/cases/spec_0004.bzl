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

// lib2.bzl
module Lib2;
import Base;

public class Invoices
  public static method Total(m: Money): long   // borrows a Base.Money
    return m.Cents;
  end Total;
end Invoices;
