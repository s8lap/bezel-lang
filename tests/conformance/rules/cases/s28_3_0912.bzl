//! case: s28_3_0912
//! doc: spec
//! section: 28.3
//! title: an implementation of a generic interface method returning a parameter not mentioning T is rejected
//! rule: together with every plain parameter whose declared type mentions `T` ... inherited by
//!   implementations and overrides
//! cites: 11.2, 14.1
//! expect: reject
//! diagnostic: source
//! issue: 8170
module Conf;

public interface IPick<T>
  method Pick(a: T, b: Array<int>): T;
end IPick;

public class Picker: IPick<Array<int>>
  public constructor()
  end constructor;

  public method Pick(a: Array<int>, b: Array<int>): Array<int> => b;
end Picker;
