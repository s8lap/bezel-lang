//! case: s14_2_4932
//! doc: spec
//! section: 14.2
//! title: a user record that lists IComparable<Rec> and declares CompareTo satisfies where T: IComparable<T>
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause
//!   and declares matching members
//! cites: 14.2, 2.8
//! expect: run
//! stdout: accepted
//! issue: 8578
module Conf;

public record Rec: IComparable<Rec>
  public method CompareTo(other: Rec): int
    return 0;
  end CompareTo;
end Rec;

public class Program
  public static method F<T>(x: T): int where T: IComparable<T>
    Console.WriteLine("accepted");
    return 0;
  end F;

  public static method Main()
    F(new Rec());
  end Main;
end Program;
