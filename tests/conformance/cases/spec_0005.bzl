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

// lib2.bzl
module Lib2;
import Base;

public class Invoices
  public static method Total(m: Money): long   // borrows a Base.Money
    return m.Cents;
  end Total;
end Invoices;

// app.bzl
module App;
import Lib1;
import Lib2;
// no `import Base;` — and none is needed for anything below

class Program
  public static method Main()
    using var pay := Payroll.Net(40, 2500);      // USE: inferred binding of a transitive type (rule 2)
    Console.WriteLine(Invoices.Total(pay));      // USE: passing the value across module surfaces
    Console.WriteLine(pay.Format());             // USE: member access
  end Main;                                      // USE: `using var` disposal of a transitive type
end Program;

// Each of the following would be NAMING, and each requires `import Base;` first:
//   var m: Money := Payroll.Net(1, 1);           — type ascription
//   using var m2 := new Money(500);              — `new`
//   if pay is Money then … end;                  — type test
//   using var box := new OwningArray<Money>();   — generic type argument
