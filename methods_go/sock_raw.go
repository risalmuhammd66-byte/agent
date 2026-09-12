package main

import (
	"syscall"
)

func openRawICMP() (int, error) {
	return syscall.Socket(syscall.AF_INET, syscall.SOCK_RAW, syscall.IPPROTO_ICMP)
}

func closeRawICMP(fd int) {
	syscall.Close(fd)
}

func rawICMPSend(fd int, addr [4]byte, pkt []byte) error {
	sa := &syscall.SockaddrInet4{Port: 0, Addr: addr}
	return syscall.Sendto(fd, pkt, 0, sa)
}