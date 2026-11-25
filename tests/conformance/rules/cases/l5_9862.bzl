//! case: l5_9862
//! doc: stdlib
//! section: 5
//! title: Max with an identity selector at a generic T := long returns the full 64-bit value
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.1
//! expect: run
//! stdout: 6000000000
//! notes: Harvested from the #8689 review (g_Max_long). U := T := long; the largest of 3000000000, 6000000000 and 5 is 6000000000, a value that does not fit in 32 bits, so a narrowing of U would change the printed number.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Max(lambda(x) => x);

  public static method Main()
    var a := new Array<long>();
    a.Add(3000000000L); a.Add(6000000000L); a.Add(5L);
    Console.WriteLine(H(a));
  end Main;
end Program;
