//! case: l5_9864
//! doc: stdlib
//! section: 5
//! title: Max over a lazy receiver with an identity selector at a generic T := string returns the string
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.1
//! expect: run
//! stdout: zzz
//! notes: Harvested from the #8689 review (q_Max_string). Max through a lazy Where receiver at U := T := string: the largest of bbb, zzz, aaa is zzz.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Where(lambda(x) => true).Max(lambda(x) => x);

  public static method Main()
    var a := new Array<string>();
    a.Add("bbb"); a.Add("zzz"); a.Add("aaa");
    Console.WriteLine(H(a));
  end Main;
end Program;
