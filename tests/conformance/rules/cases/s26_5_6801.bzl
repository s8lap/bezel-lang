//! case: s26_5_6801
//! doc: spec
//! section: 26.5
//! title: the call spelling of the ICollection property IsReadOnly through an IList receiver is refused
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5, 27.7, 4.1
//! expect: reject
//! diagnostic: instance member
//! notes: IsReadOnly is `readonly property IsReadOnly: bool` on ICollection<T> (stdlib 4.1); IList<T> extends it, so the
//!   name is declared, instance resolution commits and a bool-typed property has no call candidate (27.7).
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var c: IList<int> := a;
    var b := c.IsReadOnly();
  end Main;
end Program;
