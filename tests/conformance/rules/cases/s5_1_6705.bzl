//! case: s5_1_6705
//! doc: spec
//! section: 5.1
//! title: a transitively extended interface is a reference conversion
//! rule: **interface to extended interface**, directly or transitively
//! cites: 11.2
//! expect: accept
module Conf;

public interface IA
end IA;

public interface IB: IA
end IB;

public interface IC: IB
end IC;

public class Program
  static method G(a: IA)
  end G;
  public static method F(c: IC)
    Program.G(c);
    var b: IB := c;
  end F;
end Program;
