//! case: s23_3_8103
//! doc: spec
//! section: 23.3
//! title: a static property setter with string locals called from an iterator body
//! rule: The setter's implicit `value` parameter has no modifier by default and takes a
//!  borrow of the property's declared type.
//! cites: 23.3, 28.9
//! expect: run
//! output: 3
//!   9
//! notes: Pins #9019 (narrow) for the static, iterator-caller spelling. The iterator yields
//!  0, 1, 2 (sum 3); each setter call sees "g0", "g1", "g2" and adds the length of value +
//!  "q" (3), so Total is 9.
module Conf;
public class H
  public static Total: int := 0;
  public static property P: string
    get => "x";
    set => do
      var r := value + "q";
      H.Total := H.Total + r.Length;
    end;
  end;
end H;
public class Program
  public static method Gen(n: int): IEnumerable<int>
    for var i in 0..<n do
      var s := "g" + i.ToString();
      H.P := s;
      yield return i;
    end;
  end Gen;
  public static method Main(): int
    var tot := 0;
    for var x in Gen(3) do tot := tot + x; end;
    Console.WriteLine(tot);
    Console.WriteLine(H.Total);
    return 0;
  end Main;
end Program;
