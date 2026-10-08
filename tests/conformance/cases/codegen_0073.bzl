var a: int? := ReadOpt();
var sum := a + b;                              // ERROR;  ✓ (a ?? 0) + (b ?? 0)  or  a! + b!
if a < b then ...                                // ERROR;  ✓ if a <> null and b <> null and a! < b! then
total += price;                                // ERROR when either is nullable
count++;                                       // ERROR when count is `int?`;  ✓ count := (count ?? 0) + 1;
if (flags & ADMIN) <> 0 then Grant(); end;     // ERROR when flags is `uint?` — lifted, it would FAIL OPEN
case n of  1..5 => "small", _ => "other" end;  // ERROR when n is `int?`;  ✓ case n ?? 0 of ...
