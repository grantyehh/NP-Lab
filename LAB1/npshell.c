#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_LINE 32768
#define MAX_ARGS 256
#define MAX_CMDS 4096
#define MAX_DEFERRED 256

enum {
    PIPE_NONE = 0,
    PIPE_ORDINARY = 1,
    PIPE_NUMBERED = 2
};

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
    int pipe_mode;
    int pipe_num;
    int pipe_stderr;
} Command;

typedef struct {
    int target_command;
    int read_fd;
    int write_fd;
} DeferredPipe;

static Command command_buffer[MAX_CMDS];

static int parse_line(char *line, Command commands[]) {
    int cmd_count = 0;
    Command current;
    char *token;

    memset(&current, 0, sizeof(current));
    token = strtok(line, " ");

    while (token != NULL) {
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " ");
            if (token != NULL) {
                current.infile = token;
            }
        } else if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0) {
            current.append = (strcmp(token, ">>") == 0);
            token = strtok(NULL, " ");
            if (token != NULL) {
                current.outfile = token;
            }
        } else if (strcmp(token, "|") == 0) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_ORDINARY;
            current.pipe_stderr = 0;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } else if (strcmp(token, "!") == 0) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_ORDINARY;
            current.pipe_stderr = 1;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } else if (token[0] == '|' && isdigit((unsigned char)token[1])) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_NUMBERED;
            current.pipe_num = atoi(token + 1);
            current.pipe_stderr = 0;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } else if (token[0] == '!' && isdigit((unsigned char)token[1])) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_NUMBERED;
            current.pipe_num = atoi(token + 1);
            current.pipe_stderr = 1;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } else {
            if (current.argc < MAX_ARGS - 1) {
                current.argv[current.argc++] = token;
            }
        }

        token = strtok(NULL, " ");
    }

    if (current.argc > 0 || current.infile != NULL || current.outfile != NULL) {
        if (cmd_count >= MAX_CMDS) {
            fprintf(stderr, "too many commands in one line\n");
            return cmd_count;
        }
        current.argv[current.argc] = NULL;
        commands[cmd_count++] = current;
    }

    return cmd_count;
}

static int get_or_create_deferred_pipe(DeferredPipe deferred[], int *deferred_count, int target_command) {
    int pipefd[2];

    if (*deferred_count >= MAX_DEFERRED) {
        fprintf(stderr, "too many deferred pipes\n");
        return -1;
    }
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    deferred[*deferred_count].target_command = target_command;
    deferred[*deferred_count].read_fd = pipefd[0];
    deferred[*deferred_count].write_fd = pipefd[1];
    (*deferred_count)++;
    return *deferred_count - 1;
}

static void remove_deferred_pipe(DeferredPipe deferred[], int *deferred_count, int index) {
    int i;
    for (i = index; i + 1 < *deferred_count; ++i) {
        deferred[i] = deferred[i + 1];
    }
    (*deferred_count)--;
}

static void close_if_open(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

static void write_all(int fd, const char *buffer, ssize_t count) {
    ssize_t written = 0;
    while (written < count) {
        ssize_t result = write(fd, buffer + written, (size_t)(count - written));
        if (result <= 0) {
            break;
        }
        written += result;
    }
}

static int collect_ready_deferred_inputs(DeferredPipe deferred[], int *deferred_count,
                                         int target_command, int ready_fds[], int max_ready) {
    int ready_count = 0;
    int i = 0;

    while (i < *deferred_count) {
        if (deferred[i].target_command == target_command) {
            if (ready_count < max_ready) {
                ready_fds[ready_count++] = deferred[i].read_fd;
            } else {
                close_if_open(deferred[i].read_fd);
            }
            close_if_open(deferred[i].write_fd);
            remove_deferred_pipe(deferred, deferred_count, i);
            continue;
        }
        i++;
    }

    return ready_count;
}

static void execute_commands(Command commands[], int cmd_count, DeferredPipe deferred[],
                             int *deferred_count, int *executed_count) {
    pid_t pids[MAX_CMDS * 2];
    int pid_count = 0;
    int prev_read_fd;
    int i;

    if (cmd_count == 0) {
        return;
    }

    prev_read_fd = -1;

    for (i = 0; i < cmd_count; ++i) {
        int ordinary_pipe[2] = {-1, -1};
        int next_read_fd = -1;
        int numbered_index = -1;
        int current_command = *executed_count + 1;
        int ready_fds[MAX_DEFERRED + 1];
        int ready_count = 0;
        int stdin_fd = -1;
        int merge_pipe[2] = {-1, -1};
        int has_prev_pipe_input = (prev_read_fd >= 0);
        pid_t pid;

        if (commands[i].argc == 0 || commands[i].argv[0] == NULL) {
            close_if_open(prev_read_fd);
            prev_read_fd = -1;
            (*executed_count)++;
            continue;
        }

        if (has_prev_pipe_input) {
            ready_fds[ready_count++] = prev_read_fd;
        }
        ready_count += collect_ready_deferred_inputs(
            deferred, deferred_count, current_command, ready_fds + ready_count,
            (int)(sizeof(ready_fds) / sizeof(ready_fds[0])) - ready_count);

        if (commands[i].infile != NULL) {
            stdin_fd = -1;
            for (int j = 0; j < ready_count; ++j) {
                close_if_open(ready_fds[j]);
            }
            ready_count = 0;
        } else if (ready_count == 1) {
            stdin_fd = ready_fds[0];
        } else if (ready_count > 1) {
            if (pipe(merge_pipe) < 0) {
                perror("pipe");
                for (int j = 0; j < ready_count; ++j) {
                    close_if_open(ready_fds[j]);
                }
                close_if_open(prev_read_fd);
                prev_read_fd = -1;
                break;
            }
            pid = fork();
            if (pid < 0) {
                perror("fork");
                for (int j = 0; j < ready_count; ++j) {
                    close_if_open(ready_fds[j]);
                }
                close_if_open(merge_pipe[0]);
                close_if_open(merge_pipe[1]);
                close_if_open(prev_read_fd);
                prev_read_fd = -1;
                break;
            }
            if (pid == 0) {
                char buffer[4096];
                close_if_open(merge_pipe[0]);
                for (int j = 0; j < ready_count; ++j) {
                    ssize_t bytes;
                    while ((bytes = read(ready_fds[j], buffer, sizeof(buffer))) > 0) {
                        write_all(merge_pipe[1], buffer, bytes);
                    }
                    close_if_open(ready_fds[j]);
                }
                close_if_open(merge_pipe[1]);
                exit(0);
            }
            pids[pid_count++] = pid;
            for (int j = 0; j < ready_count; ++j) {
                close_if_open(ready_fds[j]);
            }
            stdin_fd = merge_pipe[0];
            close_if_open(merge_pipe[1]);
        }

        if (commands[i].pipe_mode == PIPE_ORDINARY) {
            if (pipe(ordinary_pipe) < 0) {
                perror("pipe");
                close_if_open(stdin_fd);
                break;
            }
            next_read_fd = ordinary_pipe[0];
        } else if (commands[i].pipe_mode == PIPE_NUMBERED) {
            numbered_index = get_or_create_deferred_pipe(
                deferred, deferred_count, current_command + commands[i].pipe_num);
            if (numbered_index < 0) {
                close_if_open(stdin_fd);
                break;
            }
        }

        pid = fork();
        if (pid < 0) {
            perror("fork");
            close_if_open(stdin_fd);
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            break;
        }

        if (pid == 0) {
            if (commands[i].infile != NULL) {
                int fdin = open(commands[i].infile, O_RDONLY);
                if (fdin < 0) {
                    perror("open input file");
                    exit(1);
                }
                dup2(fdin, STDIN_FILENO);
                close(fdin);
            } else if (stdin_fd >= 0) {
                dup2(stdin_fd, STDIN_FILENO);
            }

            if (commands[i].outfile != NULL) {
                int flags = O_WRONLY | O_CREAT | (commands[i].append ? O_APPEND : O_TRUNC);
                int fdout = open(commands[i].outfile, flags, 0644);
                if (fdout < 0) {
                    perror("open output file");
                    exit(1);
                }
                dup2(fdout, STDOUT_FILENO);
                close(fdout);
            } else if (commands[i].pipe_mode == PIPE_ORDINARY) {
                dup2(ordinary_pipe[1], STDOUT_FILENO);
                if (commands[i].pipe_stderr) {
                    dup2(ordinary_pipe[1], STDERR_FILENO);
                }
            } else if (commands[i].pipe_mode == PIPE_NUMBERED) {
                dup2(deferred[numbered_index].write_fd, STDOUT_FILENO);
                if (commands[i].pipe_stderr) {
                    dup2(deferred[numbered_index].write_fd, STDERR_FILENO);
                }
            }

            close_if_open(stdin_fd);
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            close_if_open(merge_pipe[0]);
            close_if_open(merge_pipe[1]);

            for (int j = 0; j < *deferred_count; ++j) {
                close_if_open(deferred[j].read_fd);
                if (deferred[j].write_fd != STDOUT_FILENO) {
                    close_if_open(deferred[j].write_fd);
                }
            }

            execvp(commands[i].argv[0], commands[i].argv);
            if (errno == ENOENT) {
                fprintf(stderr, "Unknown command: [%s].\n", commands[i].argv[0]);
            } else {
                perror("execvp");
            }
            exit(1);
        }

        pids[pid_count++] = pid;
        close_if_open(stdin_fd);
        prev_read_fd = next_read_fd;

        if (commands[i].pipe_mode == PIPE_ORDINARY) {
            close_if_open(ordinary_pipe[1]);
        } else {
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            prev_read_fd = -1;
        }

        (*executed_count)++;
    }

    close_if_open(prev_read_fd);

    for (i = 0; i < pid_count; ++i) {
        waitpid(pids[i], NULL, 0);
    }
}

int main(void) {
    char line[MAX_LINE];
    char original_line[MAX_LINE];
    DeferredPipe deferred[MAX_DEFERRED];
    int deferred_count = 0;
    int executed_count = 0;

    setenv("PATH", "bin:.", 1);

    while (1) {
        int cmd_count;

        printf("%% ");
        fflush(stdout);

        if (fgets(line, MAX_LINE, stdin) == NULL) {
            printf("\n");
            break;
        }

        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        strncpy(original_line, line, sizeof(original_line) - 1);
        original_line[sizeof(original_line) - 1] = '\0';

        cmd_count = parse_line(line, command_buffer);
        if (cmd_count == 0 || command_buffer[0].argc == 0 || command_buffer[0].argv[0] == NULL) {
            continue;
        }

        if (strcmp(command_buffer[0].argv[0], "exit") == 0 && cmd_count == 1) {
            break;
        }

        if (strcmp(command_buffer[0].argv[0], "cd") == 0 && cmd_count == 1) {
            if (command_buffer[0].argv[1] == NULL) {
                char *home = getenv("HOME");
                if (home == NULL) {
                    fprintf(stderr, "cd: HOME not set\n");
                } else if (chdir(home) != 0) {
                    perror("cd");
                }
            } else if (chdir(command_buffer[0].argv[1]) != 0) {
                perror("cd");
            }
            executed_count++;
            continue;
        }

        if (strcmp(command_buffer[0].argv[0], "printenv") == 0 && cmd_count == 1) {
            if (command_buffer[0].argc == 1) {
                for (char **env = environ; *env != NULL; ++env) {
                    printf("%s\n", *env);
                }
            } else {
                for (int i = 1; i < command_buffer[0].argc; ++i) {
                    int found = 0;
                    size_t len = strlen(command_buffer[0].argv[i]);
                    for (char **env = environ; *env != NULL; ++env) {
                        if (strncmp(*env, command_buffer[0].argv[i], len) == 0 && (*env)[len] == '=') {
                            printf("%s\n", *env + len + 1);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        printf("\n");
                    }
                }
            }
            executed_count++;
            continue;
        }

        if (strcmp(command_buffer[0].argv[0], "setenv") == 0 && cmd_count == 1) {
            if (command_buffer[0].argc < 3) {
                fprintf(stderr, "Usage: setenv VAR VALUE\n");
            } else if (setenv(command_buffer[0].argv[1], command_buffer[0].argv[2], 1) != 0) {
                perror("setenv");
            }
            executed_count++;
            continue;
        }

        (void)original_line;
        execute_commands(command_buffer, cmd_count, deferred, &deferred_count, &executed_count);
    }

    for (int i = 0; i < deferred_count; ++i) {
        close_if_open(deferred[i].read_fd);
        close_if_open(deferred[i].write_fd);
    }

    return 0;
}
