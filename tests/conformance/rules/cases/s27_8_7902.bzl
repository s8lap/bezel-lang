//! case: s27_8_7902
//! doc: spec
//! section: 27.8
//! title: a bound spelled through an alias of DateTime rejects a record that lists
//!   IComparable<TimeSpan>
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 14.2
//! expect: reject
//! diagnostic: constraint
module Conf;
import System;

type MyDT := DateTime;

public record Rec: IComparable<TimeSpan>
  public method CompareTo(other: TimeSpan): int
    return 0;
  end CompareTo;
end Rec;

public class Program
  public static method F<T>(x: T): int where T: IComparable<MyDT>
    return 1;
  end F;
  public static method Main(): int
    Console.WriteLine(F(new Rec()));
    return 0;
  end Main;
end Program;
