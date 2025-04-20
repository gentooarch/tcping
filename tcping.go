package main

import (
        "fmt"
        "net"
        "os"
        "strconv"
        "sync"
        "time"
)

type ThreadArgs struct {
        id    int
        count int
        host  string
        port  int
}

func tcpPing(args ThreadArgs, wg *sync.WaitGroup) {
        defer wg.Done()
        address := fmt.Sprintf("%s:%d", args.host, args.port)

        for i := 0; i < args.count; i++ {
                start := time.Now()
                conn, err := net.DialTimeout("tcp", address, 5*time.Second)
                elapsed := time.Since(start)

                if err != nil {
                if err != nil {
                        fmt.Printf("Thread %d - Attempt %d: Connection failed: %s\n", args.id, i+1, err.Error())
                } else {
                        remoteAddr := conn.RemoteAddr().String()
                        ip, _, _ := net.SplitHostPort(remoteAddr)
                        fmt.Printf("Thread %d - Attempt %d: Connected to %s (%s):%d, time=%d ms\n",
                                args.id, i+1, args.host, ip, args.port, elapsed.Milliseconds())
                        conn.Close()
                }
                time.Sleep(500 * time.Millisecond)
        }
}

func main() {
        threads := 8
        count := 1
        port := 443
        var host string

        args := os.Args
        if len(args) == 2 {
                host = args[1]
        } else if len(args) == 3 {
                threads, _ = strconv.Atoi(args[1])
                host = args[2]
        } else if len(args) == 4 {
                threads, _ = strconv.Atoi(args[1])
                count, _ = strconv.Atoi(args[2])
                host = args[3]
        } else if len(args) == 5 {
                threads, _ = strconv.Atoi(args[1])
                count, _ = strconv.Atoi(args[2])
                port, _ = strconv.Atoi(args[3])
                host = args[4]
        } else {
                fmt.Printf("Usage: %s [threads] [count] [port] hostname\n", args[0])
                return
        }

        var wg sync.WaitGroup
        for i := 0; i < threads; i++ {
                wg.Add(1)
                go tcpPing(ThreadArgs{
                        id:    i,
                        count: count,
                        host:  host,
                        port:  port,
                }, &wg)
        }
        wg.Wait()
}
