//! case: s5_1_6702
//! doc: spec
//! section: 5.1
//! title: a user interface extending IFormattable converts to IFormattable
//! rule: **interface to extended interface**, directly or transitively
//! cites: 11.2
//! expect: accept
module Conf;

public interface IFmt: IFormattable
end IFmt;

public class Program
  static method G(f: IFormattable)
  end G;
  public static method F(f: IFmt)
    Program.G(f);
  end F;
end Program;
