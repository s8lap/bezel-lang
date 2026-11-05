//! case: s5_1_0912
//! doc: spec
//! section: 5.1
//! title: a nullable int from ?. on a JsonOptions? is not passed to Console.WriteLine's Object parameter
//! rule: A nullable `T?` is none of these kinds: by the lifting rule above it converts at most to
//!   `Object?`, never to `Object`
//! cites: 7.5, 2.15
//! expect: reject
//! diagnostic: never to Object
//! issue: 7966
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9);
    Console.WriteLine(o?.MaxDepth);
  end Main;
end Program;
