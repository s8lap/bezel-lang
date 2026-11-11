//! case: s14_2_4802
//! doc: spec
//! section: 14.2
//! title: Array<bool>.Sort() is rejected: Boolean is declared without IComparable
//! rule: A primitive conforms to the interfaces its standard-library declaration lists
//! cites: stdlib 9.4, 4.4
//! expect: reject
//! diagnostic: requires T: IComparable<T>
module Conf;

public class Program
  public static method Main()
    var a: Array<bool> := [true, false];
    a.Sort();
  end Main;
end Program;
