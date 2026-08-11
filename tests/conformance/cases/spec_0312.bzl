case move outcome of
  Fetched(var r)       => do Validate(r); Publish(move r); end,    // Validate may throw
  NotModified          => (),
  Failed(var s, var m) => throw new UpstreamException(m)
end;
