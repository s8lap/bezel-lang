//! case: l5_1307
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on an iterator method's IEnumerable returns the element or null, at record and
//!   long
//! rule: null when index is out of range
//! cites: 15.1, 29.3
//! expect: run
//! output: 4
//!   -1
//!   20000000000
//!   -1
module Conf;

record P(X: int, Y: int);

public class Program
  static method Gen(): IEnumerable<P>
    yield return new P(1, 2);
    yield return new P(3, 4);
  end Gen;

  static method GenL(): IEnumerable<long>
    yield return 10000000000L;
    yield return 20000000000L;
  end GenL;

  static method Pk(e: IEnumerable<P>, i: int): int
    var p := e.ElementAtOrNull(i);
    if p = null then
      return -1;
    end if;
    return p!.Y;
  end Pk;

  public static method Main()
    Console.WriteLine(Pk(Gen(), 1));
    Console.WriteLine(Pk(Gen(), 2));
    Console.WriteLine(GenL().ElementAtOrNull(1) ?? -1);
    Console.WriteLine(GenL().ElementAtOrNull(-1) ?? -1);
  end Main;
end Program;
