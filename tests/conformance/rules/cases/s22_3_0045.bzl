//! case: s22_3_0045
//! doc: spec
//! section: 22.3
//! title: a lambda's written ref parameter of an owning choice is a compile error
//! rule: A ref parameter whose declared type is a move-only value type
//!   (§15.3; an owning choice's nullable wrapper C? among them, §35.9), a
//!   function type's ref slot included, is a compile error at the
//!   declaration
//! cites: 22.3, 27.1
//! expect: reject
//! diagnostic: move-only value type
//! issue: 5986
module Conf;
class Res
    public constructor() end constructor;
end Res;
choice Bundle
    Ready(owned first: Res),
    Empty;
end Bundle;
class C
    public static method Main(): int
        var f := lambda(ref b: Bundle) => do end;
        return 0;
    end Main;
end C;
