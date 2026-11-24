//! case: s27_8_7903
//! doc: spec
//! section: 27.8
//! title: an alias-of-an-alias bound admits a class instantiation of the aliased target's
//!   conformer
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 14.2
//! expect: run
//! stdout: 5
module Conf;
import System;

type Stamp := DateTime;
type Moment := Stamp;

public record Rec: IComparable<DateTime>
  public method CompareTo(other: DateTime): int
    return 0;
  end CompareTo;
end Rec;

public class Box<T> where T: IComparable<Moment>
  public method N(): int => 5;
end Box;

public class Program
  public static method Main(): int
    var b := new Box<Rec>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
