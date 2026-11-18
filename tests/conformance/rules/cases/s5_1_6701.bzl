//! case: s5_1_6701
//! doc: spec
//! section: 5.1
//! title: a user interface extending IAsyncDisposable converts to IAsyncDisposable
//! rule: **interface to extended interface**, directly or transitively
//! cites: 11.2, 28.12
//! expect: accept
module Conf;

public interface IFoo: IAsyncDisposable
end IFoo;

public class Program
  static method G(d: IAsyncDisposable)
  end G;
  public static method F(f: IFoo)
    Program.G(f);
  end F;
end Program;
