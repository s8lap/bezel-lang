public choice FetchOutcome
  Fetched(owned response: Response),      // owned class payload
  NotModified,                            // payload-free case
  Failed(status: int, message: string);   // value payloads

  public method IsSuccess(): bool => (case this of
    Fetched(_)   => true,
    NotModified  => true,
    Failed(_, _) => false);
end FetchOutcome;
