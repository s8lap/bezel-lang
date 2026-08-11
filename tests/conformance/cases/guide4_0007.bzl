using var t := sem.AcquireAsync();     // OwningTask<SemaphoreToken>
using var tok := await t;              // first award: the token is yours
using var tok2 := await t;             // runtime InvalidOperationException: already spent
