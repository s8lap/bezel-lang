//! case: s14_2_0075
//! doc: spec
//! section: 14.2
//! title: an ordinary interface does not guarantee a class requirement at the head of an extension
//! rule: an ordinary interface's conforming set is open, so an interface
//!   bound excludes only what a language rule excludes
//! cites: 14.2, 14.4, 26.4
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 5549
module Conf;
interface IShape method Area(): int; end IShape;
extension OwningArray<T> where T: IShape
  public method P(): int return 7; end P;
end;
class C public static method Main(): int return 0; end Main; end C;
