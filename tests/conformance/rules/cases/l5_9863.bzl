//! case: l5_9863
//! doc: stdlib
//! section: 5
//! title: Min over a lazy receiver with an identity selector at a generic T := long returns the long
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.1
//! expect: run
//! stdout: 5
//! notes: Harvested from the #8689 review (q_Min_long). The same selector Min, reached through a lazy Where receiver (a deferred query consumed once by Min): U := T := long, the least of the three is 5.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Where(lambda(x) => true).Min(lambda(x) => x);

  public static method Main()
    var a := new Array<long>();
    a.Add(3000000000L); a.Add(6000000000L); a.Add(5L);
    Console.WriteLine(H(a));
  end Main;
end Program;
