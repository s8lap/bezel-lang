//! case: s3_6_0002
//! doc: spec
//! section: 3.6
//! title: default of a non-nullable reference type is a compile error
//! rule: Not defaultable: non-nullable reference types.
//! cites: 3.6, 17.11, 4.6
//! expect: reject
//! diagnostic: defaultable
module Conf;

public class Holder
  public method Compute()
    var s: string := default(string);
  end Compute;
end Holder;
