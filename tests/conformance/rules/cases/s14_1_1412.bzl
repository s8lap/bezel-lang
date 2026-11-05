//! case: s14_1_1412
//! doc: spec
//! section: 14.1
//! title: StringBuilder and TimeSpan members keep their types in classes with type parameters of those names
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! output: 4
//!   5
//!   4
module Conf;
public class Holder
  public owned B: StringBuilder;
  public property T: TimeSpan;
  public constructor()
    B := new StringBuilder();
    B.Append("abcd");
    T := TimeSpan.FromTicks(5L);
  end constructor;
end Holder;
public class Util
  public static method Len(b: StringBuilder): int
    return b.Length;
  end Len;
  public static method Tk(t: TimeSpan): long
    return t.Ticks;
  end Tk;
end Util;
public class Box<StringBuilder>
  public method Go(h: Holder): int
    return Util.Len(h.B);
  end Go;
end Box;
public class Box2<TimeSpan>
  public method Go(h: Holder): long
    return Util.Tk(h.T);
  end Go;
end Box2;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>();
    Console.WriteLine(b.Go(h));
    var c := new Box2<int>();
    Console.WriteLine(c.Go(h));
    var d := new Box<string>();
    Console.WriteLine(d.Go(h));
  end Main;
end Program;
