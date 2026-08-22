// ConnectAsync is illustrative; see §28.12 note.
await using var connection := await ConnectAsync(url);
await connection.SendAsync(data);
// At end of scope: connection.DisposeAsync() awaited.
