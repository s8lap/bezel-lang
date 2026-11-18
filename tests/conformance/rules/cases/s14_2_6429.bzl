//! case: s14_2_6429
//! doc: spec
//! section: 14.2
//! title: an alias-spelled IComparable<DT> bound still rejects a record lacking
//!   IComparable<DateTime>
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
module Conf;
type DT := DateTime;
public class Program
  public static method Main(): int
    return F(new Rec());
  end Main;
  public static method F<T>(x: T): int where T: IComparable<DT>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public record Rec: IComparable<Rec>
  public method CompareTo(other: Rec): int
    return 0;
  end CompareTo;
end Rec;
