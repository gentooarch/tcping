CGO_ENABLED=0  go build  -o tcping tcping.go
gcc -o tcping tcping.c -lpthread

