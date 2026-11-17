//! case: l4_4_2517
//! doc: stdlib
//! section: 4.4
//! title: Sort() on a method-local record that declares CompareTo, in a plain and in a generic method
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: 2:10
//!   3:20
//!   1:30
//!   2:10
//!   3:20
//!   1:30
//! issue: 8584
module Conf;

public class Program
  public static method Run()
    record W(tag: int, v: int): IComparable<W>
      public method CompareTo(other: W): int
        if v < other.v then return -1; end if;
        if v > other.v then return 1; end if;
        return 0;
      end CompareTo;
    end W;
    using var xs := new Array<W>();
    xs.Add(new W(1, 30));
    xs.Add(new W(2, 10));
    xs.Add(new W(3, 20));
    xs.Sort();
    for var e in xs do
      Console.WriteLine($"{e.tag}:{e.v}");
    end;
  end Run;

  public static method RunG<T>(t: T)
    record W(tag: int, v: int): IComparable<W>
      public method CompareTo(other: W): int
        if v < other.v then return -1; end if;
        if v > other.v then return 1; end if;
        return 0;
      end CompareTo;
    end W;
    using var xs := new Array<W>();
    xs.Add(new W(1, 30));
    xs.Add(new W(2, 10));
    xs.Add(new W(3, 20));
    xs.Sort();
    for var e in xs do
      Console.WriteLine($"{e.tag}:{e.v}");
    end;
  end RunG;

  public static method Main()
    Run();
    RunG<int>(1);
  end Main;
end Program;
