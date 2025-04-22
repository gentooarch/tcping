package main

import (
	"encoding/csv"
	"flag"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

// Result holds the IP address and the latency of a successful ping.
type Result struct {
	IP      string
	Latency time.Duration
}

// pingTCP attempts a single TCP connection and returns the latency.
// Returns -1 duration if the connection fails or times out.
func pingTCP(ip string, port string, timeout time.Duration) time.Duration {
	start := time.Now()
	conn, err := net.DialTimeout("tcp", net.JoinHostPort(ip, port), timeout)
	if err != nil {
		// Indicate failure with a negative duration
		return -1
	}
	conn.Close()
	return time.Since(start)
}

// scanWorker reads IPs from ipChan, pings them, and sends results to resultChan.
func scanWorker(ipChan <-chan string, resultChan chan<- Result, port string, timeout time.Duration, wg *sync.WaitGroup) {
	for ip := range ipChan {
		latency := pingTCP(ip, port, timeout)
		// Send result regardless of success, filtering happens in the writer
		resultChan <- Result{IP: ip, Latency: latency}
		wg.Done() // Signal completion for this IP
	}
}

// isCIDR checks if the input string looks like a CIDR notation.
func isCIDR(input string) bool {
	return strings.Contains(input, "/")
}

// parseCIDR parses a CIDR string and returns a slice of usable host IP addresses.
// It attempts to exclude the network and broadcast addresses.
func parseCIDR(cidr string) []string {
	var ipList []string
	ip, ipnet, err := net.ParseCIDR(cidr)
	if err != nil {
		fmt.Printf("CIDR parsing failed: %v\n", err)
		return ipList // Return empty list on error
	}
	// Iterate through all IPs in the network defined by the mask
	for ip := ip.Mask(ipnet.Mask); ipnet.Contains(ip); incIP(ip) {
		ipCopy := make(net.IP, len(ip))
		copy(ipCopy, ip)
		ipList = append(ipList, ipCopy.String())
	}
	// Remove network and broadcast addresses - This simple slicing is only
	// reliable for certain common subnet masks (like /24 to /30).
	// It will fail for /31, /32 and potentially others.
	// A more robust implementation is needed for general cases.
	if len(ipList) > 2 {
		return ipList[1 : len(ipList)-1]
	}
	// For smaller ranges (like /31 or /32), return the generated list as is.
	return ipList
}

// incIP increments the IP address (handles IPv4 and IPv6).
func incIP(ip net.IP) {
	for j := len(ip) - 1; j >= 0; j-- {
		ip[j]++
		if ip[j] > 0 { // Stop incrementing if carry doesn't propagate
			break
		}
	}
}

// startWriter creates/opens result.csv and writes successful results from the channel.
func startWriter(wg *sync.WaitGroup, results <-chan Result) {
	// Create the file, overwriting if it exists
	file, err := os.Create("result.csv")
	if err != nil {
		fmt.Printf("Could not create CSV file: %v\n", err)
		// If file creation fails, we should still drain the channel
		// and decrement the WaitGroup to prevent deadlock.
		go func() {
			for range results {
				wg.Done()
			}
		}()
		return
	}
	defer file.Close()

	writer := csv.NewWriter(file)
	defer writer.Flush() // Ensure all buffered data is written

	// Write CSV header
	header := []string{"IP", "Latency(ms)"}
	if err := writer.Write(header); err != nil {
		fmt.Printf("Error writing CSV header: %v\n", err)
		// Continue trying to write data rows even if header fails
	}

	// Read results from the channel
	for result := range results {
		// Only write successful results (latency >= 0)
		if result.Latency >= 0 {
			latencyMsStr := fmt.Sprintf("%.2f", result.Latency.Seconds()*1000)
			record := []string{result.IP, latencyMsStr}
			if err := writer.Write(record); err != nil {
				fmt.Printf("Error writing record to CSV for IP %s: %v\n", result.IP, err)
				// Log error but continue processing other results
			}
		}
		// Signal that this result has been processed by the writer
		wg.Done()
	}
}

// pingSingleIP performs multiple pings to a single IP address concurrently.
func pingSingleIP(ip string, port string, count int, timeout time.Duration, threads int) {
	var wg sync.WaitGroup        // WaitGroup for CSV writer tasks
	var pingWg sync.WaitGroup   // WaitGroup for ping goroutines
	results := make(chan Result, count) // Buffered channel for results

	// Start the CSV writer goroutine
	// It needs to process 'count' potential results.
	wg.Add(count) // Expect 'count' calls to wg.Done() from the writer
	go startWriter(&wg, results)

	// Use a semaphore to limit concurrency to 'threads'
	sem := make(chan struct{}, threads)

	// Launch 'count' ping attempts
	pingWg.Add(count)
	for i := 0; i < count; i++ {
		sem <- struct{}{} // Acquire semaphore slot
		go func() {
			defer func() {
				<-sem         // Release semaphore slot
				pingWg.Done() // Mark this ping attempt as complete
			}()
			latency := pingTCP(ip, port, timeout)
			// Send result to the channel for the writer to process
			results <- Result{IP: ip, Latency: latency}
		}()
	}

	// Wait for all ping goroutines to finish sending results
	pingWg.Wait()
	// Close the results channel to signal the writer that no more results are coming
	close(results)
	// Wait for the CSV writer to finish processing all results it received
	wg.Wait()
}

// scanCIDR scans all usable IPs in a CIDR range concurrently.
func scanCIDR(cidr string, port string, threads int, timeout time.Duration) {
	ipList := parseCIDR(cidr)
	if len(ipList) == 0 {
		fmt.Println("CIDR parsing resulted in an empty or unusable IP list")
		return
	}

	numIPs := len(ipList)
	ipChan := make(chan string, numIPs)      // Channel to send IPs to workers
	resultChan := make(chan Result, numIPs) // Channel to receive results from workers
	var workerWg sync.WaitGroup            // WaitGroup for worker goroutines
	var writerWg sync.WaitGroup            // WaitGroup for the CSV writer

	// Start worker goroutines
	workerWg.Add(numIPs) // Expect 'numIPs' calls to workerWg.Done() from workers
	for i := 0; i < threads; i++ {
		go scanWorker(ipChan, resultChan, port, timeout, &workerWg)
	}

	// Start the CSV writer goroutine
	writerWg.Add(numIPs) // Expect 'numIPs' calls to writerWg.Done() from the writer
	go startWriter(&writerWg, resultChan)

	// Send IPs to the workers
	for _, ip := range ipList {
		ipChan <- ip
	}
	close(ipChan) // Signal workers that no more IPs are coming

	// Wait for all worker goroutines to finish processing IPs
	workerWg.Wait()
	// Close the result channel to signal the writer that no more results are coming
	close(resultChan)
	// Wait for the writer to finish processing all results
	writerWg.Wait()
}

func main() {
	// Define command-line flags
	ip := flag.String("ip", "", "Single IP address")
	cidr := flag.String("cidr", "", "Address range in CIDR format")
	port := flag.String("port", "80", "Target port")
	count := flag.Int("count", 4, "Number of pings (for single IP mode)")
	threads := flag.Int("threads", 50, "Number of concurrent threads/workers")
	timeout := flag.Duration("timeout", 1*time.Second, "Connection timeout duration")

	// Parse the flags
	flag.Parse()

	// Validate flags
	if *ip == "" && *cidr == "" {
		fmt.Println("Error: Must specify either -ip or -cidr")
		flag.Usage() // Print default usage message
		return
	}

	if *ip != "" && *cidr != "" {
		fmt.Println("Error: Cannot specify both -ip and -cidr")
		flag.Usage()
		return
	}

	// Execute based on mode
	if *ip != "" {
		// Single IP mode
		fmt.Printf("Starting TCP ping for %s:%s...\n", *ip, *port)
		pingSingleIP(*ip, *port, *count, *timeout, *threads)
		fmt.Printf("Ping complete for %s:%s. Results saved to result.csv\n", *ip, *port)
	} else {
		// CIDR scan mode
		fmt.Printf("Starting scan for %s on port %s...\n", *cidr, *port)
		scanCIDR(*cidr, *port, *threads, *timeout)
		fmt.Printf("Scan complete for %s:%s. Results saved to result.csv\n", *cidr, *port)
	}
}
