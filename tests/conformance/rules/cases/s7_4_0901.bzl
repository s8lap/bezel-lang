//! case: s7_4_0901
//! doc: spec
//! section: 7.4
//! title: a JsonOptions? call result bound to an un-narrowed local does not admit a member read
//! rule: Member access, method calls, and indexing on a nullable value are compile errors unless the
//!   compiler has narrowed it to non-null
//! cites: 2.15
//! expect: reject
//! diagnostic: nullable
//! issue: 7966
module Conf;

import System.Serialization;

public class Program
  public static method Mk(b: bool): JsonOptions?
    if b then
      return new JsonOptions(true, 4, false, true, 9);
    end if;
    return null;
  end Mk;

  public static method Main()
    var o := Mk(true);
    Console.WriteLine(o.MaxDepth);
  end Main;
end Program;
