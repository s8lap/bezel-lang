var name: string := "Alice";       // cannot be null
var maybe: string? := null;        // explicitly nullable
var n := name.Length;              // fine
var m := maybe.Length;             // ERROR: maybe might be null
