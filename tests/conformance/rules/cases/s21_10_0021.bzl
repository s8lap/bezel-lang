//! case: s21_10_0021
//! doc: spec
//! section: 21.10
//! title: a local record method returns its own local record type and folds an array argument
//! rule: "except that a local type's scope also covers its own body (below)."
//! cites: 9
//! expect: run
//! output: 13
module Conf;

public class Program
  public static method Main()
    var xs := new Array<int>();
    xs.Add(1); xs.Add(2);
    record Tot(sum: int)
      public method Plus(ys: Array<int>): Tot
        var s := sum;
        for var y in ys do s := s + y; end;
        return new Tot(s);
      end Plus;
    end Tot;
    Console.WriteLine(new Tot(10).Plus(xs).sum);
  end Main;
end Program;
