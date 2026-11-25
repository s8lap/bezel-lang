//! case: l5_9860
//! doc: stdlib
//! section: 5
//! title: Max with an identity selector at a generic T := double returns the double
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.1
//! expect: run
//! stdout: 2.25
//! notes: Harvested from the #8689 review (r43-rev-m150-8689, g_Max_double). The generic body's selector is `(T) -> U` with U := T; at T := double the result is the double 2.25 (the largest of 1.5, 2.25, 0.5), U read through the substitution of 3.6 rule 2, not narrowed to int. The int instantiation of the same helper is the unchanged control.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Max(lambda(x) => x);

  public static method Main()
    var a := new Array<double>();
    a.Add(1.5); a.Add(2.25); a.Add(0.5);
    Console.WriteLine(H(a));
  end Main;
end Program;
