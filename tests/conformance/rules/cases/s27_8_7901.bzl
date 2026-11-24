//! case: s27_8_7901
//! doc: spec
//! section: 27.8
//! title: a bound spelled through an alias of DateTime admits a record that lists
//!   IComparable<DateTime>
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 14.2
//! expect: run
//! stdout: 1
module Conf;
import System;

type MyDT := DateTime;

public record Rec: IComparable<DateTime>
  public method CompareTo(other: DateTime): int
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
