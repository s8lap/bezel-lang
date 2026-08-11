_ := DoTelemetryAsync();          // ERROR: a task's discard would skip its
                                  //   continuation cleanup (spec §17.13, §30.10)
