//! case: l5_9861
//! doc: stdlib
//! section: 5
//! title: Min with an identity selector at a generic T := string returns the string
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.1
//! expect: run
//! stdout: aaa
//! notes: Harvested from the #8689 review (g_Min_string). U := T := string; the least of bbb, zzz, aaa under string's IComparable<string> is aaa. The selector yields the element itself, so no fresh string is minted.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Min(lambda(x) => x);

  public static method Main()
    var a := new Array<string>();
    a.Add("bbb"); a.Add("zzz"); a.Add("aaa");
    Console.WriteLine(H(a));
  end Main;
end Program;
