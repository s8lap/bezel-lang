//! case: s5_1_6704
//! doc: spec
//! section: 5.1
//! title: IAsyncEnumerator<int> converts to IAsyncDisposable at a local declaration
//! rule: **interface to extended interface**, directly or transitively
//! cites: 11.2
//! expect: accept
module Conf;

public class Program
  public static method F(e: IAsyncEnumerator<int>)
    var d: IAsyncDisposable := e;
  end F;
end Program;
