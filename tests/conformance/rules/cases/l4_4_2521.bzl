//! case: l4_4_2521
//! doc: stdlib
//! section: 4.4
//! title: Sort() is rejected on a record that has no CompareTo and does not conform to IComparable
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 8584
module Conf;

public record Plain(a: int, b: int);

public class Program
  public static method Main()
    var a: Array<Plain> := [new Plain(2, 1), new Plain(1, 2)];
    a.Sort();
  end Main;
end Program;
