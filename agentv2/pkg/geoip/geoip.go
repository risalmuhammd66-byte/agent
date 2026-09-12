package geoip

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type TargetGeoInfo struct {
	ResolvedIP string
	ISP        string
	Region     string
	Country    string
	ASN        string
}

var httpClient = &http.Client{
	Timeout: 3 * time.Second,
}

func Resolve(rawHost string) TargetGeoInfo {
	info := TargetGeoInfo{
		ResolvedIP: "N/A",
		ISP:        "Unknown ISP",
		Region:     "Unknown Region",
		Country:    "Unknown Country",
		ASN:        "Unknown ASN",
	}

	host := strings.TrimSpace(rawHost)
	if strings.HasPrefix(strings.ToLower(host), "http://") || strings.HasPrefix(strings.ToLower(host), "https://") {
		if u, err := url.Parse(host); err == nil && u.Host != "" {
			host = u.Hostname()
		}
	}

	// Clean port if present e.g. 1.2.3.4:80
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}

	ip := net.ParseIP(host)
	if ip != nil {
		info.ResolvedIP = ip.String()
	} else {
		addrs, err := net.LookupHost(host)
		if err == nil && len(addrs) > 0 {
			info.ResolvedIP = addrs[0]
		} else {
			info.ResolvedIP = host
		}
	}

	if info.ResolvedIP != "N/A" && net.ParseIP(info.ResolvedIP) != nil {
		apiURL := fmt.Sprintf("http://ip-api.com/json/%s?fields=status,country,regionName,isp,as", info.ResolvedIP)
		resp, err := httpClient.Get(apiURL)
		if err == nil {
			defer resp.Body.Close()
			var data struct {
				Status     string `json:"status"`
				Country    string `json:"country"`
				RegionName string `json:"regionName"`
				ISP        string `json:"isp"`
				AS         string `json:"as"`
			}
			if err := json.NewDecoder(resp.Body).Decode(&data); err == nil && data.Status == "success" {
				if data.Country != "" {
					info.Country = data.Country
				}
				if data.RegionName != "" {
					info.Region = data.RegionName
				}
				if data.ISP != "" {
					info.ISP = data.ISP
				}
				if data.AS != "" {
					info.ASN = data.AS
				}
			}
		}
	}

	return info
}
