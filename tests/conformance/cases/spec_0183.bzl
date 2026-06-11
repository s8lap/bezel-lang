type UserId = int;
type ConnectionPool = Dictionary<string, Array<Connection>>;

/// <summary>Handles an HTTP response.</summary>
/// <param name="status">The HTTP status code.</param>
/// <returns>True if handled successfully.</returns>
type ResponseHandler = (status: int, body: string, elapsed: TimeSpan) -> bool;
