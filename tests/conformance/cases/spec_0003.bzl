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

// lib1.bzl
module Lib1;
import Base;

public class Payroll
  public static method Net(hours: int, centsPerHour: int): owned Money
    return new Money(hours * centsPerHour);   // fresh owned Base.Money, crossing Lib1's surface
  end Net;
end Payroll;
