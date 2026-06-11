var len := maybe?.Length;                       // len: int?  — null if maybe is null
var display: string := maybe ?? "Anonymous";    // substitute a default
var forced := maybe!;                           // assert non-null (throws if wrong)
