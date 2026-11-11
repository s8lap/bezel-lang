// The same four situations, each in an "expected here" and a "must not happen here" form.
// Go: an error is a value; nothing in its type says whether it is expected. The caller
// decides at each call: handle it, return it, or stop (log.Fatal / panic / a Must helper).
package main

import (
	"errors"
	"fmt"
	"io/fs"
	"log"
	"net"
	"os"
	"regexp"
	"strconv"
	"time"
)

const deadAddr = "127.0.0.1:1" // nothing listens here: connection refused

func call(addr string) error {
	c, err := net.DialTimeout("tcp", addr, 300*time.Millisecond)
	if err != nil {
		return err
	}
	return c.Close()
}

// 1a. A file the user chose. Missing is expected: say so and carry on.
func openUserFile(path string) {
	data, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) { // the handler tests the error, by value
		fmt.Println("user-file: no such file, pick another")
		return
	}
	if err != nil {
		fmt.Println("user-file: cannot read:", err)
		return
	}
	fmt.Println("user-file:", len(data), "bytes")
}

// 1b. The database the service cannot run without. Same call, same error value.
// This caller decides it is fatal.
func openRequiredFile(path string) []byte {
	data, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("required-file: cannot start: %v", err) // exit status 1
	}
	return data
}

// 2a. Optional telemetry: the failure is dropped at the call.
func sendTelemetry() {
	if err := call(deadAddr); err != nil {
		fmt.Println("telemetry: dropped one sample")
	}
}

// 2b. The upstream this service exists to call: the failure is returned to the boundary.
func handleRequest() error {
	if err := call(deadAddr); err != nil {
		return fmt.Errorf("upstream: %w", err)
	}
	return nil
}

// 3a. A number the user typed.
func parseUser(s string) {
	n, err := strconv.Atoi(s)
	if err != nil {
		fmt.Println("user-number: not a number, ask again")
		return
	}
	fmt.Println("user-number:", n)
}

// 3b. A pattern and a number the program wrote. The standard library ships a second
// entry point for this stance: regexp.Compile returns an error, regexp.MustCompile panics.
var datePattern = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}$`)

func mustAtoi(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		panic(fmt.Sprintf("constant-number: bad literal %q: %v", s, err))
	}
	return n
}

// 4a. A key the user supplied: the two-value form.   4b. A key the program put there itself.
var limits = map[string]int{"retries": 3}

func lookupUser(k string) {
	if v, ok := limits[k]; ok {
		fmt.Println("user-key:", v)
	} else {
		fmt.Println("user-key: unknown setting")
	}
}

func lookupOwn(k string) int {
	v, ok := limits[k]
	if !ok {
		panic("own-key: missing built-in setting " + k)
	}
	return v
}

func main() {
	log.SetFlags(0)
	switch os.Args[1] {
	case "user-file":
		openUserFile("/nonexistent/notes.txt")
	case "required-file":
		openRequiredFile("/nonexistent/service.db")
	case "telemetry":
		sendTelemetry()
	case "primary-api":
		if err := handleRequest(); err != nil { // the request boundary
			fmt.Println("primary-api: 502 Bad Gateway")
		}
	case "user-number":
		parseUser("12a")
	case "constant-number":
		fmt.Println("constant-number:", mustAtoi("8080"), datePattern.MatchString("2026-11-10"))
	case "constant-number-wrong":
		fmt.Println(mustAtoi("80a0"))
	case "user-key":
		lookupUser("colour")
	case "own-key":
		fmt.Println("own-key:", lookupOwn("retries"))
	case "own-key-wrong":
		fmt.Println(lookupOwn("retires"))
	}
}
