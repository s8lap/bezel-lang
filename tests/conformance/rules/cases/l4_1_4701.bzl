//! case: l4_1_4701
//! doc: stdlib
//! section: 4.1
//! title: ?.Add on a nullable IReadOnlyList<T> is rejected because the interface has no Add
//! rule: public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
//! cites: 4.1, 7.4, 7.5
//! expect: reject
//! diagnostic: no member `Add` on `IReadOnlyList`
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [7, 8, 9];
    var c: IReadOnlyList<int>? := a;
    c?.Add(4);
  end Main;
end Program;
