# my_shell_project-

Overview

This project is a custom Unix-like shell implemented in C. It provides a command-line interface where users can run programs, manage I/O redirection, chain processes with pipes, and control foreground/background execution. It was built to deepen understanding of system calls, process management, signals, and inter-process communication in Unix-based operating systems
.

Features

Command parsing

Handles multiple arguments and commands.

Supports up to 10 piped commands in a single line.

I/O Redirection

< redirect input from a file.

> redirect output to a file (overwrite).

>> redirect output to a file (append).

Pipes (|)

Chain multiple commands together where the output of one command becomes the input of the next.

Background Execution (&)

Run processes in the background without blocking the shell.

Foreground Process Control

Tracks and waits for foreground processes to finish before returning to the prompt.

Signal Handling

Cleans up terminated background processes using SIGCHLD handler.

Error Handling

Detects missing filenames for redirection.

Handles invalid commands and long input safely.
