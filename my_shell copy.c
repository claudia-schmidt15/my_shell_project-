
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define MAX_ARGS 100
#define MAX_CMD_LENGTH 1024
#define MAX_CMDS 10

typedef struct {
    char **args;           // Argument vector
    char *input_file;      // Input redirection file
    char *output_file;     // Output redirection file
    int append;            // Flag for appending output (1 if '>>' is used)
    int background;        // Flag for background execution
} Cmd;

typedef struct {
    Cmd commands[MAX_CMDS];
    int num_commands;
} CmdSet;

//Track PIDs of foreground processes
pid_t foreground_pids[MAX_CMDS];  
int num_foreground_pids = 0;

//Function declarations
char *getCmd(const char *program_name);
CmdSet parse_command(char *cmd);
void execute_commands(CmdSet *cmdset);
void handle_foreground_pids();
void cleanup_stray_processes();
void execute_single_command(Cmd *cmd, int input_fd, int output_fd);
void setup_redirection(Cmd *cmd);
void signal_handler(int signo);
char **get_tokens(const char *line);
void free_tokens(char **tokens);

int main(int argc, char *argv[]) {
    const char *program_name = argv[0];
    signal(SIGCHLD, signal_handler);  // Handle terminated background processes

    while (1) {
        //Get command input
        char *cmd = getCmd(program_name);

        if(cmd == NULL){
            continue;
        }   

        //Parse and execute commands
        CmdSet cmdset = parse_command(cmd);

        if (cmdset.num_commands > 0) {
            //Execute parsed commands
            execute_commands(&cmdset);
        }

        //Wait for foreground processes to finish 
        handle_foreground_pids();  
    }

    //Kill stray processes on exit
    cleanup_stray_processes();  
    return 0;
}

// Get command input from user
char *getCmd(const char *program_name) {
    static char cmd[MAX_CMD_LENGTH];
    char *result;

    //Print the prompt 
    printf("mysh: ");
    //Ensure prompt is printed immediately 
    fflush(stdout);
    result = fgets(cmd, sizeof(cmd), stdin);
    if (result == NULL) {
        if(strlen(cmd) >= 1024){
            exit(0); 
        }
        
        if(feof(stdin)){
            cleanup_stray_processes();
            exit(0);
        }else{
            fprintf(stderr, "Error: Usage: %s [prompt]\n", program_name);
            exit(0);
        }
    }
    return cmd;
}

//Parse input into a CmdSet structure
CmdSet parse_command(char *cmd) {
    //Check for too long command
    if (strlen(cmd) >= 1024) { 
        exit(0);
    }
    
    //Starts with 0 commands
    CmdSet cmdset = { .num_commands = 0 };

    //Splits input string into separate words
    char **tokens = get_tokens(cmd);
    //If no tokens...
    if(tokens[0] == NULL){
        free_tokens(tokens);
        return cmdset;
    }

    //Initialize cmd struct. Represents a single command
    Cmd current_cmd = { .args = NULL, .input_file = NULL, .output_file = NULL, .append = 0, .background = 0 };
    //Stores arguments of current command
    char **args_buffer = malloc(MAX_ARGS * sizeof(char *));
    //Tracks number of arguments 
    int arg_index = 0;

    //Iterates over arguments 
    for (int i = 0; tokens[i] != NULL; i++) {
        
        if (strcmp(tokens[i], "&") == 0) {
            //Command should be run in background
            current_cmd.background = 1;
        } else if (strcmp(tokens[i], "<") == 0) {
            i++;
            //Next token must be input file name
            if (tokens[i] == NULL) {
                fprintf(stderr, "Error: Missing filename for input redirection.\n");
                break;
            }
            //Creates copy of input file name and stores it in current_cmd.input_file
            current_cmd.input_file = strdup(tokens[i]);
        } else if (strcmp(tokens[i], ">") == 0) {
            i++;
            //Next token must be input file name
            if (tokens[i] == NULL) {
                fprintf(stderr, "Error: Missing filename for output redirection.\n");
                break;
            }
            current_cmd.output_file = strdup(tokens[i]);

            //Overwrite operation
            current_cmd.append = 0;
        } else if (strcmp(tokens[i], ">>") == 0) {
            i++;
            if (tokens[i] == NULL) {
                fprintf(stderr, "Error: Missing filename for output redirection.\n");
                break;
            }
            current_cmd.output_file = strdup(tokens[i]);

            //Indicates data should be appended to the file instead of overwriting it
            current_cmd.append = 1;
        } else if (strcmp(tokens[i], "|") == 0) {
            args_buffer[arg_index] = NULL;
            current_cmd.args = malloc((arg_index + 1) * sizeof(char *));
            memcpy(current_cmd.args, args_buffer, (arg_index + 1) * sizeof(char *));
            cmdset.commands[cmdset.num_commands++] = current_cmd;

            current_cmd = (Cmd) { .args = NULL, .input_file = NULL, .output_file = NULL, .append = 0, .background = 0 };
            arg_index = 0;
        } else {
            args_buffer[arg_index++] = strdup(tokens[i]);
        }
    }

    if (arg_index > 0) {
        args_buffer[arg_index] = NULL;
        current_cmd.args = malloc((arg_index + 1) * sizeof(char *));
        memcpy(current_cmd.args, args_buffer, (arg_index + 1) * sizeof(char *));
        cmdset.commands[cmdset.num_commands++] = current_cmd;
    }

    free(args_buffer);
    free_tokens(tokens);
    return cmdset;
}

//Execute all commands in a CmdSet
void execute_commands(CmdSet *cmdset) {
    //Keepts track of input file descriptor 
    int input_fd = STDIN_FILENO;
    //Holds fds used for piping between processes
    int pipe_fd[2];
    
    //Iterates over each command in the command set 
    for (int i = 0; i < cmdset->num_commands; i++) {
        
        //Account for long pipeline
        if (i >= 128) {
            break;  
        }
        
        Cmd *cmd = &cmdset->commands[i];

        //Check for empty commands
        if (cmd == NULL || cmd->args == NULL || cmd->args[0] == NULL || strlen(cmd->args[0]) == 0 || strlen(cmd->args[0]) >= 1024) {
            continue;  
    }

        //Setup pipe if necessary. Checks if current command is not the last comment in the set
        if (i < cmdset->num_commands - 1) {
            //If true, creates a pipe. pipe_fd[0] for reading and pipe[1] for writing.
            if (pipe(pipe_fd) < 0) {
                perror("Error creating pipe");
                return;
            }
        }

        //Executes the command. cmd is the current command.
        execute_single_command(cmd, input_fd, i < cmdset->num_commands - 1 ? pipe_fd[1] : STDOUT_FILENO);

        if (input_fd != STDIN_FILENO) {
            close(input_fd);
        }

        //If current command is not the last, closes pipe_fd[1]. 
        //Updates input_fd to pipe_fd[0] so next command can read from output of current command.
        if (i < cmdset->num_commands - 1) {
            close(pipe_fd[1]);
            input_fd = pipe_fd[0];
        }
    }
}

//Execute a single command
void execute_single_command(Cmd *cmd, int input_fd, int output_fd) {
    //Check for empty command
    if (cmd == NULL || cmd->args == NULL || cmd->args[0] == NULL || strlen(cmd->args[0]) == 0 || strlen(cmd->args[0]) >= 1024) {
        return;  
    }
    
    //Creates a child process 
    pid_t pid = fork();

    //Child process
    if (pid == 0) { 
        setup_redirection(cmd);

        if (input_fd != STDIN_FILENO) {
            //Duplicates input_fd so that child's standard input is now input_fd
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        if (output_fd != STDOUT_FILENO) {
            //Redirects the standard output to output_fd
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }

        //Replaces current process with new process
        execvp(cmd->args[0], cmd->args);

    //Parent process 
    } else if(pid > 0){ 
        if(!cmd->background) {
            //Child process runs in foreground
            foreground_pids[num_foreground_pids++] = pid;
        }else{
            printf("[Background PID %d]\n", pid);
        }
    } else{
        perror("fork failed");
    }
}

//Setup input/output redirection
void setup_redirection(Cmd *cmd){
    //Checks if command has an input file specified 
    if(cmd->input_file){
        //Opens the file in read mode 
        int fd = open(cmd->input_file, O_RDONLY);
        if(fd < 0){
            fprintf(stderr, "Error: open(\"%s\"): %s\n", cmd->input_file, strerror(errno));
            exit(0);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    //Checks if command has an output file specified
    if(cmd->output_file){
        int fd = open(cmd->output_file, O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC), S_IRUSR | S_IWUSR);
        if(fd < 0){
            fprintf(stderr, "Error: open(\"%s\"): %s\n", cmd->output_file, strerror(errno));
            exit(0);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

//Wait for all foreground processes to terminate
void handle_foreground_pids() {
    //Stores status info of a terminated child process
    int status;
    //Stores the pid of the terminated process returned by wait()
    pid_t pid;

    //Wait for each foreground process
    while (num_foreground_pids > 0) {
        //Wait for any child process to terminate
        pid = wait(&status);  
        if (pid == -1) {
            if (errno == ECHILD) {
                //No more children to wait for
                break;
            } else {
                perror("wait failed");
                break;
            }
        }

        //Check if the terminated child was a foreground process
        for(int i = 0; i < num_foreground_pids; i++){
            if(foreground_pids[i] == pid){
                //Shift remaining PIDs left in the array
                for(int j = i; j < num_foreground_pids - 1; j++){
                    foreground_pids[j] = foreground_pids[j + 1];
                }
                num_foreground_pids--;
                break;
            }
        }
    }
}

// Cleanup any stray processes before exiting the shell
void cleanup_stray_processes() {
    int status;
    while (wait3(&status, WNOHANG, NULL) > 0);
}

// Handle terminated background processes
void signal_handler(int signo) {
    if (signo == SIGCHLD) {
        int status;
        while (wait3(&status, WNOHANG, NULL) > 0);
    }
}


