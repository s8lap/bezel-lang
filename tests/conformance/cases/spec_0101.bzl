var description := (case score of
  > 90 => "excellent",
  > 70 => "good",
  > 50 => "passing",
  _    => Classify(score)            // helper for the multi-step case
);

private static method Classify(score: int): string
  Log("low score: " + score.ToString());
  return (if score < 0 then "invalid" else "failing");
end Classify;
