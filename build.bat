@echo off
rem Needs Go (https://go.dev/dl/). Builds ItemCmd.exe from main.go + index.html.
go build -ldflags "-s -w" -o ItemCmd.exe .
