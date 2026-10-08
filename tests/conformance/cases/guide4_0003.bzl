_ := DoTelemetryAsync();            // ERROR: disposing a task abandons its
                                    //   running operation (spec §17.13, §30.10)
using var _ := DoTelemetryAsync();  // ERROR too: nothing can await an
                                    //   unnamed binding, so the form is the
                                    //   discard's abandonment one scope later
                                    //   (spec §17.13; `await using var _` alike)
