//! case: s5_1_6703
//! doc: spec
//! section: 5.1
//! title: IAsyncEnumerator<int> converts to IAsyncDisposable as an argument
//! rule: **interface to extended interface**, directly or transitively
//! cites: 11.2
//! expect: accept
module Conf;

public class Program
  static method G(d: IAsyncDisposable)
  end G;
  public static method F(e: IAsyncEnumerator<int>)
    Program.G(e);
  end F;
end Program;
