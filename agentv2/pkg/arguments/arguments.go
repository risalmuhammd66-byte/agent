package arguments

import (
	"flag"
	"fmt"
	"os"
)

type ConfigArgs struct {
	Port        int
	UsersFile   string
	MethodsFile string
}

func Parse() ConfigArgs {
	var cfg ConfigArgs

	fs := flag.NewFlagSet("agentv2", flag.ContinueOnError)
	fs.IntVar(&cfg.Port, "p", 1337, "Unified port for SSH and Bots")
	fs.IntVar(&cfg.Port, "port", 1337, "Unified port for SSH and Bots")
	fs.StringVar(&cfg.UsersFile, "users", "users.json", "Path to users.json")
	fs.StringVar(&cfg.MethodsFile, "methods", "methods.json", "Path to methods.json")

	if err := fs.Parse(os.Args[1:]); err != nil {
		fmt.Printf("[!] Argument parse warning: %v\n", err)
	}

	return cfg
}
