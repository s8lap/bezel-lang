//! case: l4_4_1903
//! doc: stdlib
//! section: 4.4
//! title: a user record named Guid without IComparable is not sortable by natural order
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
module Conf;

record Guid(N: int);

class Program
  public static method Main()
    var a: Array<Guid> := [new Guid(2), new Guid(1)];
    a.Sort();
  end Main;
end Program;
