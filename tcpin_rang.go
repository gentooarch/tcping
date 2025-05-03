package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

// tcpPing 尝试与给定的 IP 和端口建立 TCP 连接
// 只有成功或非超时的失败结果会被发送到 results channel
func tcpPing(ip string, port string, wg *sync.WaitGroup, results chan<- string) {
	defer wg.Done() // 在函数结束时调用 Done()

	address := fmt.Sprintf("%s:%s", ip, port)
	start := time.Now() // 在尝试连接前记录时间

	// 尝试建立 TCP 连接，设置超时时间为 100 毫秒 (或其他你设置的值)
	conn, err := net.DialTimeout("tcp", address, 100*time.Millisecond)
	duration := time.Since(start)

	if err == nil {
		// --- 连接成功 ---
		conn.Close() // 立即关闭连接
		result := fmt.Sprintf("IP: %s:%s 连接成功 延迟: %v\n", ip, port, duration)
		results <- result // 发送成功结果到 channel
	} else {
		// --- 连接失败 ---
		// 检查失败原因是否 *不是* 超时
		if netErr, ok := err.(net.Error); !ok || !netErr.Timeout() {
			// 如果错误不是网络错误(少见)，或者 是网络错误但不是超时错误 (例如 connection refused, no route to host)
			result := fmt.Sprintf("IP: %s:%s 连接失败: %v\n", ip, port, err)
			results <- result // 发送非超时的失败结果到 channel
		}
		// 如果是超时错误 (netErr.Timeout() 为 true), 则不执行任何操作，
		// 即不将结果发送到 results channel。
	}
}

func main() {
	// --- 命令行参数定义 ---
	var baseIP string
	var port string
	var startSubnet int
	var endSubnet int
	var outputFile string
	var concurrency int

	flag.StringVar(&baseIP, "ip", "172.64", "基础IP网段 (例如: 172.64, 192.168.1)")
	flag.IntVar(&startSubnet, "start", 0, "起始的第三个八位字节 (0-255)")
	flag.IntVar(&endSubnet, "end", 255, "结束的第三个八位字节 (0-255)")
	flag.StringVar(&port, "port", "443", "要测试的TCP端口")
	flag.StringVar(&outputFile, "o", "tcp_ping_results.txt", "结果输出文件")
	flag.IntVar(&concurrency, "c", 100, "并发 goroutine 数量")
	flag.Parse()

	// --- 参数验证 ---
	if startSubnet < 0 || startSubnet > 255 || endSubnet < 0 || endSubnet > 255 || startSubnet > endSubnet {
		fmt.Println("错误：起始/结束子网必须在 0-255 之间，且起始不大于结束。")
		flag.Usage()
		os.Exit(1)
	}
	if concurrency <= 0 {
		fmt.Println("错误：并发数必须大于 0。")
		os.Exit(1)
	}

	// --- BaseIP 输入处理与验证 ---
	originalBaseIP := baseIP // 保存原始输入用于提示
	parts := strings.Split(baseIP, ".")
	if len(parts) < 2 || len(parts) > 3 {
		fmt.Printf("错误：提供的 -ip '%s' 格式无效。\n", originalBaseIP)
		fmt.Println("       请输入类似 '192.168' 或 '10.0.5' 的基础网段。")
		flag.Usage()
		os.Exit(1)
	}

	// --- 文件操作 ---
	file, err := os.Create(outputFile)
	if err != nil {
		fmt.Printf("错误：无法创建文件 '%s': %v\n", outputFile, err)
		os.Exit(1)
	}
	defer file.Close()
	fmt.Printf("结果将写入到 %s (超时的连接将被忽略)\n", outputFile) // 更新提示信息

	// --- 并发处理 ---
	var wg sync.WaitGroup
	// Channel 大小设为 concurrency 比较合理，避免在写入goroutine处理慢时阻塞ping goroutine
	// 但如果所有都成功/失败（非超时），理论上最大需要 end-start+1
	// 取一个折中或较大的值，或者就用 endSubnet-startSubnet+1 保证不阻塞
	results := make(chan string, endSubnet-startSubnet+1)
	semaphore := make(chan struct{}, concurrency)

	// 启动一个 goroutine 来收集并写入结果
	go func() {
		for result := range results {
			_, err := file.WriteString(result)
			if err != nil {
				fmt.Fprintf(os.Stderr, "警告：写入文件失败: %v\n", err)
			}
		}
	}()

	// --- 执行 TCP Ping ---
	fmt.Printf("开始扫描 %s.%d.1 到 %s.%d.1 端口 %s (并发数: %d)...\n",
		baseIP, startSubnet, baseIP, endSubnet, port, concurrency)

	for i := startSubnet; i <= endSubnet; i++ {
		ipToPing := fmt.Sprintf("%s.%d.1", baseIP, i)

		wg.Add(1)
		semaphore <- struct{}{} // 获取令牌

		go func(targetIP string) {
			defer func() { <-semaphore }() // 释放令牌
			tcpPing(targetIP, port, &wg, results)
		}(ipToPing)
	}

	// --- 等待与清理 ---
	wg.Wait()      // 等待所有 ping goroutine 完成
	close(results) // 关闭 results channel，通知写入 goroutine 结束

	fmt.Println("扫描完成。")
}
