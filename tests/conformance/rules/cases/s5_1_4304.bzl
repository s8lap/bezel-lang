//! case: s5_1_4304
//! doc: spec
//! section: 5.1
//! title: a user record named Guid that declares no interface does not convert to IComparable<Guid>
//! rule: a type to an interface it implements or conforms to
//! cites: 5.1, 2.8
//! expect: reject
//! diagnostic: has a type that is not implicitly convertible
module Conf;

public record Guid(x: int)
end Guid;

public class Program
  public static method Main()
    var u := new Guid(3);
    var g: IComparable<Guid> := u;
  end Main;
end Program;
