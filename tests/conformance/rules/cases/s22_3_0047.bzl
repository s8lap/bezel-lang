//! case: s22_3_0047
//! doc: spec
//! section: 22.3
//! title: a function type's ref slot is admitted for a nullable copyable choice and a nullable owning class
//! rule: A ref parameter whose declared type is a move-only value type
//!   (§15.3; an owning choice's nullable wrapper C? among them, §35.9), a
//!   function type's ref slot included, is a compile error at the
//!   declaration
//! cites: 22.3, 27.1
//! expect: run
//! output: ran
module Conf;
class Res
    public constructor() end constructor;
end Res;
choice Color
    Red,
    Green;
end Color;
class C
    public static method Main(): int
        var f: ((ref Color?) -> ())? := null;
        var g: ((ref Res?) -> ())? := null;
        Console.WriteLine("ran");
        return 0;
    end Main;
end C;
