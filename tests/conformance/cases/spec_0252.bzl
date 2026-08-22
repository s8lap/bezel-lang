var cfg := await FetchAsync(url);        // the scoped bind…

using var task2 := FetchAsync(url);      // …desugars to exactly this pair,
var cfg2 := await task2;                 //    the slot fresh and inexpressible
