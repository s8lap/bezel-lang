//! case: s28_3_8_7206
//! doc: spec
//! section: 28.3.8
//! title: a capture-free function value converted to an Object-typed local
//!   is admitted
//! rule: At every set-erasing position a function value with a non-empty
//!   source set is rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: accept
module Conf;

public class Client
  public static method Run()
    var f: () -> int := lambda() => 1;
    var o: Object := f;
  end Run;
end Client;
