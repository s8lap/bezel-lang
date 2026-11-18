//! case: s5_1_6713
//! doc: spec
//! section: 5.1
//! title: assigning an interface-typed value to a class-typed field is not an implicit conversion
//! rule: The following conversions are implicit
//! cites: 12.7
//! expect: reject
//! diagnostic: implicit
module Conf;

public interface IMark
end IMark;

public class Holder
  public ex: Exception?;
  public method Set(i: IMark)
    this.ex := i;
  end Set;
end Holder;

public class Program
  public static method Main()
    Console.WriteLine("x");
  end Main;
end Program;
