module Lib1;
import Base;

public class Payroll
  public static method Net(hours: int, centsPerHour: int): owned Money
    return new Money(hours * centsPerHour);
  end Net;
end Payroll;
