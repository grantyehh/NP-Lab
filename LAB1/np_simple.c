#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_LINE 32768
#define MAX_ARGS 256
#define MAX_CMDS 4096
#define MAX_DEFERRED 256
#define DEFAULT_BATCH_THRESHOLD 128
#define DEFAULT_BATCH_SIZE 32
#define MIN_BATCH_THRESHOLD 32
#define MAX_RELAY_BYTES (16 * 1024 * 1024)

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
    int target_slot;
    int read_fd;
    int write_fd;
} DeferredPipe;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuffer;

typedef struct {
    DeferredPipe deferred[MAX_DEFERRED];
    int deferred_count;
    int current_slot;
} ShellState;

static Command command_buffer[MAX_CMDS];
static void execute_commands(Command commands[], int cmd_count, DeferredPipe deferred[],
                             int *deferred_count, int current_line);

static int parse_line(char *line, Command commands[]) { // parse command, return how many command, and store in commands array
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
        } 
        else if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0) {
            current.append = (strcmp(token, ">>") == 0);
            token = strtok(NULL, " ");
            if (token != NULL) {
                current.outfile = token;
            }
        } 
        else if (strcmp(token, "|") == 0) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_ORDINARY;
            current.pipe_stderr = 0;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } 
        else if (strcmp(token, "!") == 0) {
            if (cmd_count >= MAX_CMDS) {
                fprintf(stderr, "too many commands in one line\n");
                return cmd_count;
            }
            current.pipe_mode = PIPE_ORDINARY;
            current.pipe_stderr = 1;
            current.argv[current.argc] = NULL;
            commands[cmd_count++] = current;
            memset(&current, 0, sizeof(current));
        } 
        else if (token[0] == '|' && isdigit((unsigned char)token[1])) {
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
        } 
        else if (token[0] == '!' && isdigit((unsigned char)token[1])) {
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
        } 
        else {
            if (current.argc < MAX_ARGS - 1) {
                current.argv[current.argc++] = token; //take token and store in argv array
            }
        }

        token = strtok(NULL, " "); //get next token
    }

    if (current.argc > 0 || current.infile != NULL || current.outfile != NULL) { //last command
        if (cmd_count >= MAX_CMDS) {
            fprintf(stderr, "too many commands in one line\n");
            return cmd_count;
        }
        current.argv[current.argc] = NULL;
        commands[cmd_count++] = current;
    }

    return cmd_count;
}

static int get_or_create_deferred_pipe(DeferredPipe deferred[], int *deferred_count, int target_slot) {
    int pipefd[2];

    for (int i = 0; i < *deferred_count; ++i) {
        if (deferred[i].target_slot == target_slot) {
            return i;
        }
    }

    if (*deferred_count >= MAX_DEFERRED) {
        fprintf(stderr, "too many deferred pipes\n");
        return -1;
    }
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    deferred[*deferred_count].target_slot = target_slot;
    deferred[*deferred_count].read_fd = pipefd[0];
    deferred[*deferred_count].write_fd = pipefd[1];
    (*deferred_count)++;
    return *deferred_count - 1;
}

static void remove_deferred_pipe(DeferredPipe deferred[], int *deferred_count, int index) {
    for (int i = index; i + 1 < *deferred_count; ++i) {
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
                                         int target_slot, int ready_fds[], int max_ready) {
    int ready_count = 0;
    int i = 0;

    while (i < *deferred_count) {
        if (deferred[i].target_slot == target_slot) {
            if (ready_count < max_ready) {
                ready_fds[ready_count++] = deferred[i].read_fd;
            } 
            else {
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

static int count_line_segments(const Command commands[], int cmd_count) {
    int segments = 1;

    for (int i = 0; i + 1 < cmd_count; ++i) {
        if (commands[i].pipe_mode == PIPE_NUMBERED) {
            segments++;
        }
    }

    return segments;
}

static int is_builtin_name(const char *name) {
    return strcmp(name, "exit") == 0 || strcmp(name, "cd") == 0 || strcmp(name, "setenv") == 0 ||
           strcmp(name, "printenv") == 0 || strcmp(name, "source") == 0;
}

static int parse_positive_env(const char *name, int fallback) {
    char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (value == NULL || *value == '\0') {
        return fallback;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 1000000L) {
        return fallback;
    }

    return (int)parsed;
}

static int get_fallback_threshold(void) {
    struct rlimit limit;
    int threshold = DEFAULT_BATCH_THRESHOLD;

    if (getrlimit(RLIMIT_NPROC, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        int current_limit = (int)limit.rlim_cur;
        int half = current_limit / 2;
        int minus_margin = current_limit - 32;

        threshold = (half < minus_margin) ? half : minus_margin;
        if (threshold < MIN_BATCH_THRESHOLD) {
            threshold = MIN_BATCH_THRESHOLD;
        }
    }

    return parse_positive_env("NP_PIPE_BATCH_THRESHOLD", threshold);
}

static int get_batch_size(void) {
    return parse_positive_env("NP_PIPE_BATCH_SIZE", DEFAULT_BATCH_SIZE);
}

static int should_use_batched_pipeline(const Command commands[], int cmd_count) {
    int threshold = get_fallback_threshold();

    if (cmd_count <= threshold) {
        return 0;
    }

    for (int i = 0; i < cmd_count; ++i) {
        const Command *command = &commands[i];
        int is_last = (i == cmd_count - 1);

        if (command->argc == 0 || command->argv[0] == NULL) {
            return 0;
        }
        if (command->infile != NULL || command->outfile != NULL || command->append ||
            command->pipe_stderr || is_builtin_name(command->argv[0])) {
            return 0;
        }
        if (!is_last && command->pipe_mode != PIPE_ORDINARY) {
            return 0;
        }
        if (is_last && command->pipe_mode != PIPE_NONE) {
            return 0;
        }
    }

    return 1;
}

static void free_buffer(ByteBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static int ensure_buffer_capacity(ByteBuffer *buffer, size_t needed) {
    char *new_data;
    size_t new_cap;

    if (needed <= buffer->cap) {
        return 0;
    }
    if (needed > MAX_RELAY_BYTES) {
        fprintf(stderr, "batched pipeline buffer exceeded limit\n");
        return -1;
    }

    new_cap = (buffer->cap == 0) ? 4096 : buffer->cap;
    while (new_cap < needed) {
        if (new_cap >= MAX_RELAY_BYTES / 2) {
            new_cap = MAX_RELAY_BYTES;
            break;
        }
        new_cap *= 2;
    }

    new_data = realloc(buffer->data, new_cap);
    if (new_data == NULL) {
        perror("realloc");
        return -1;
    }

    buffer->data = new_data;
    buffer->cap = new_cap;
    return 0;
}

static int append_buffer(ByteBuffer *buffer, const char *data, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (ensure_buffer_capacity(buffer, buffer->len + len) != 0) {
        return -1;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return 0;
}

static int read_fd_to_buffer(int fd, ByteBuffer *buffer) {
    char chunk[4096];
    ssize_t bytes;

    buffer->len = 0;
    while ((bytes = read(fd, chunk, sizeof(chunk))) > 0) {
        if (append_buffer(buffer, chunk, (size_t)bytes) != 0) {
            return -1;
        }
    }

    if (bytes < 0) {
        perror("read");
        return -1;
    }

    return 0;
}

static int run_pipeline_batch(Command commands[], int start, int end, const ByteBuffer *input,
                              ByteBuffer *output) {
    pid_t pids[MAX_CMDS];
    int pid_count = 0;
    int prev_read_fd = -1;
    int input_pipe[2] = {-1, -1};
    int capture_pipe[2] = {-1, -1};
    int use_input_pipe = (input != NULL);

    output->len = 0;

    if (use_input_pipe && pipe(input_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    if (pipe(capture_pipe) < 0) {
        perror("pipe");
        close_if_open(input_pipe[0]);
        close_if_open(input_pipe[1]);
        return -1;
    }

    prev_read_fd = use_input_pipe ? input_pipe[0] : -1;

    for (int i = start; i < end; ++i) {
        int ordinary_pipe[2] = {-1, -1};
        int next_read_fd = -1;
        int stdin_fd = prev_read_fd;
        int is_last = (i == end - 1);
        pid_t pid;

        if (!is_last && pipe(ordinary_pipe) < 0) {
            perror("pipe");
            close_if_open(prev_read_fd);
            close_if_open(capture_pipe[0]);
            close_if_open(capture_pipe[1]);
            close_if_open(input_pipe[0]);
            close_if_open(input_pipe[1]);
            return -1;
        }

        if (!is_last) {
            next_read_fd = ordinary_pipe[0];
        }

        pid = fork();
        if (pid < 0) {
            perror("fork");
            close_if_open(stdin_fd);
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            close_if_open(capture_pipe[0]);
            close_if_open(capture_pipe[1]);
            close_if_open(input_pipe[1]);
            for (int j = 0; j < pid_count; ++j) {
                waitpid(pids[j], NULL, 0);
            }
            return -1;
        }

        if (pid == 0) {
            if (stdin_fd >= 0) {
                dup2(stdin_fd, STDIN_FILENO);
            }

            if (is_last) {
                dup2(capture_pipe[1], STDOUT_FILENO);
            } else {
                dup2(ordinary_pipe[1], STDOUT_FILENO);
            }

            close_if_open(stdin_fd);
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            close_if_open(capture_pipe[0]);
            close_if_open(capture_pipe[1]);
            close_if_open(input_pipe[0]);
            close_if_open(input_pipe[1]);

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
        if (!is_last) {
            close_if_open(ordinary_pipe[1]);
        }
        prev_read_fd = next_read_fd;
    }

    close_if_open(prev_read_fd);
    close_if_open(capture_pipe[1]);
    close_if_open(input_pipe[0]);

    if (use_input_pipe) {
        if (input->len > 0) {
            write_all(input_pipe[1], input->data, (ssize_t)input->len);
        }
        close_if_open(input_pipe[1]);
    }

    if (read_fd_to_buffer(capture_pipe[0], output) != 0) {
        close_if_open(capture_pipe[0]);
        for (int i = 0; i < pid_count; ++i) {
            waitpid(pids[i], NULL, 0);
        }
        return -1;
    }

    close_if_open(capture_pipe[0]);

    for (int i = 0; i < pid_count; ++i) {
        waitpid(pids[i], NULL, 0);
    }

    return 0;
}

static int execute_commands_batched(Command commands[], int cmd_count) {
    ByteBuffer current = {0};
    ByteBuffer next = {0};
    const ByteBuffer *input = NULL;
    int batch_size = get_batch_size();
    int start = 0;

    while (start < cmd_count) {
        int end = start + batch_size;

        if (end > cmd_count) {
            end = cmd_count;
        }

        if (run_pipeline_batch(commands, start, end, input, &next) != 0) {
            free_buffer(&current);
            free_buffer(&next);
            return -1;
        }

        free_buffer(&current);
        current = next;
        next.data = NULL;
        next.len = 0;
        next.cap = 0;
        input = &current;
        start = end;
    }

    if (current.len > 0) {
        write_all(STDOUT_FILENO, current.data, (ssize_t)current.len);
    }
    free_buffer(&current);
    return 0;
}

static int run_script_file(const char *path, ShellState *state);

static int process_command_line(char *line, ShellState *state) {
    int cmd_count;

    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') {
        return 0;
    }

    cmd_count = parse_line(line, command_buffer);
    if (cmd_count == 0 || command_buffer[0].argc == 0 || command_buffer[0].argv[0] == NULL) {
        return 0;
    }

    if (strcmp(command_buffer[0].argv[0], "exit") == 0 && cmd_count == 1) {
        return 1;
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
        state->current_slot += count_line_segments(command_buffer, cmd_count);
        return 0;
    }

    if (strcmp(command_buffer[0].argv[0], "printenv") == 0 && cmd_count == 1) {
        if (command_buffer[0].argc == 1) {
            for (char **env = environ; *env != NULL; ++env) {
                printf("%s\n", *env);
            }
        } else {
            for (int i = 1; i < command_buffer[0].argc; ++i) {
                size_t len = strlen(command_buffer[0].argv[i]);

                for (char **env = environ; *env != NULL; ++env) {
                    if (strncmp(*env, command_buffer[0].argv[i], len) == 0 && (*env)[len] == '=') {
                        printf("%s\n", *env + len + 1);
                        break;
                    }
                }
            }
        }
        state->current_slot += count_line_segments(command_buffer, cmd_count);
        return 0;
    }

    if (strcmp(command_buffer[0].argv[0], "setenv") == 0 && cmd_count == 1) {
        if (command_buffer[0].argc < 3) {
            fprintf(stderr, "Usage: setenv VAR VALUE\n");
        } else if (setenv(command_buffer[0].argv[1], command_buffer[0].argv[2], 1) != 0) {
            perror("setenv");
        }
        state->current_slot += count_line_segments(command_buffer, cmd_count);
        return 0;
    }

    if (strcmp(command_buffer[0].argv[0], "source") == 0 && cmd_count == 1) {
        int should_exit = 0;

        if (command_buffer[0].argc < 2) {
            fprintf(stderr, "Usage: source FILE\n");
        } else {
            should_exit = run_script_file(command_buffer[0].argv[1], state);
        }
        state->current_slot += count_line_segments(command_buffer, cmd_count);
        return should_exit;
    }

    if (should_use_batched_pipeline(command_buffer, cmd_count)) {
        if (execute_commands_batched(command_buffer, cmd_count) != 0) {
            fprintf(stderr, "batched pipeline execution failed\n");
        }
    } else {
        execute_commands(command_buffer, cmd_count, state->deferred, &state->deferred_count, state->current_slot);
    }
    state->current_slot += count_line_segments(command_buffer, cmd_count);
    return 0;
}

static int run_script_file(const char *path, ShellState *state) {
    FILE *file = fopen(path, "r");
    char line[MAX_LINE];

    if (file == NULL) {
        perror("source");
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (process_command_line(line, state)) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static void execute_commands(Command commands[], int cmd_count, DeferredPipe deferred[],
                             int *deferred_count, int current_line) {
    pid_t pids[MAX_CMDS * 2];
    int pid_should_wait[MAX_CMDS * 2];
    int pid_count = 0;
    int prev_read_fd = -1;
    int last_segment_slot;
    int should_wait_last_segment;

    if (cmd_count == 0) {
        return;
    }

    last_segment_slot = current_line + count_line_segments(commands, cmd_count) - 1;
    should_wait_last_segment = (commands[cmd_count - 1].pipe_mode != PIPE_NUMBERED);

    for (int i = 0; i < cmd_count; ++i) {
        int ordinary_pipe[2] = {-1, -1};
        int next_read_fd = -1;
        int numbered_index = -1;
        int ready_fds[MAX_DEFERRED + 1];
        int ready_count = 0;
        int stdin_fd = -1;
        int merge_pipe[2] = {-1, -1};
        int has_prev_pipe_input = (prev_read_fd >= 0);
        int command_slot = current_line;
        pid_t pid;

        if (commands[i].argc == 0 || commands[i].argv[0] == NULL) {
            close_if_open(prev_read_fd);
            prev_read_fd = -1;
            continue;
        }

        if (has_prev_pipe_input) {
            ready_fds[ready_count++] = prev_read_fd;
        }
        ready_count += collect_ready_deferred_inputs(
            deferred, deferred_count, command_slot, ready_fds + ready_count,
            (int)(sizeof(ready_fds) / sizeof(ready_fds[0])) - ready_count);

        if (commands[i].infile != NULL) {
            for (int j = 0; j < ready_count; ++j) {
                close_if_open(ready_fds[j]);
            }
            ready_count = 0;
        } 
        else if (ready_count == 1) { //one pipe input, use it as stdin
            stdin_fd = ready_fds[0];
        } 
        else if (ready_count > 1) { //more than one pipe input, merge them into one pipe
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
            pid_should_wait[pid_count - 1] =
                should_wait_last_segment && (command_slot == last_segment_slot);
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
                deferred, deferred_count, command_slot + commands[i].pipe_num);
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
            } 
            else if (stdin_fd >= 0) {
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
            }
            else if (commands[i].pipe_mode == PIPE_ORDINARY) {
                dup2(ordinary_pipe[1], STDOUT_FILENO);
                if (commands[i].pipe_stderr) {
                    dup2(ordinary_pipe[1], STDERR_FILENO);
                }
            }
            else if (commands[i].pipe_mode == PIPE_NUMBERED) {
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
            }
            else {
                perror("execvp");
            }
            exit(1);
        }

        pids[pid_count++] = pid;
        pid_should_wait[pid_count - 1] =
            should_wait_last_segment &&
            (command_slot == last_segment_slot ||
             (commands[i].pipe_mode == PIPE_NUMBERED &&
              command_slot + commands[i].pipe_num == last_segment_slot));
        close_if_open(stdin_fd);
        prev_read_fd = next_read_fd;

        if (commands[i].pipe_mode == PIPE_ORDINARY) {
            close_if_open(ordinary_pipe[1]);
        }
        else {
            close_if_open(ordinary_pipe[0]);
            close_if_open(ordinary_pipe[1]);
            prev_read_fd = -1;
        }

        if (commands[i].pipe_mode == PIPE_NUMBERED) {
            current_line++;
        }
    }

    close_if_open(prev_read_fd);

    if (should_wait_last_segment) {
        for (int i = 0; i < pid_count; ++i) {
            if (pid_should_wait[i]) {
                waitpid(pids[i], NULL, 0);
            }
        }
    }
    else {
        for (int i = 0; i < pid_count; ++i) {
            int status;

            if (waitpid(pids[i], &status, WNOHANG) == 0) {
                continue;
            }
        }

        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
    }
}

static void run_shell_session(void) {
    char line[MAX_LINE];
    ShellState state;

    memset(&state, 0, sizeof(state));
    state.current_slot = 1;
    setenv("PATH", "bin:.", 1);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    while (1) {
        printf("%% ");
        fflush(stdout);

        if (fgets(line, MAX_LINE, stdin) == NULL) {
            break;
        }
        if (process_command_line(line, &state)) {
            break;
        }
    }

    for (int i = 0; i < state.deferred_count; ++i) {
        close_if_open(state.deferred[i].read_fd);
        close_if_open(state.deferred[i].write_fd);
    }
}

static void reap_children(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static int create_server_socket(unsigned short port) {
    int listen_fd; //socket file descriptor(operate on the socket)
    int enable = 1;
    struct sockaddr_in server_addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {  //establish reuseaddr socket
        perror("setsockopt");
        close(listen_fd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; //IPv4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port); //transform little endian to big endian

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

int main(int argc, char *argv[]) {
    int listen_fd;
    struct sigaction sa;
    unsigned short port;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port = (unsigned short)atoi(argv[1]);
    if (port == 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    //set up signal handler for SIGCHLD, avoid zombie processes
    // call by kernel when child process terminates
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN); //ignore SIGPIPE signal, prevent SIGPIPE when writing to a closed pipe

    listen_fd = create_server_socket(port);
    if (listen_fd < 0) {
        return 1;
    }

    while (1) {
        int conn_fd;
        pid_t pid;

        conn_fd = accept(listen_fd, NULL, NULL); //accept a connection request from a client
        if (conn_fd < 0) {
            if (errno == EINTR) { //interrupt by signal
                continue;
            }
            perror("accept"); //print error message
            continue;
        }

        pid = fork(); //create a new process
        if (pid < 0) {
            perror("fork"); //print error message
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            close(listen_fd); // child does not need the listening socket
            signal(SIGCHLD, SIG_DFL);
            dup2(conn_fd, STDIN_FILENO);
            dup2(conn_fd, STDOUT_FILENO);
            dup2(conn_fd, STDERR_FILENO);
            close(conn_fd);
            run_shell_session();
            exit(0);
        }

        close(conn_fd);
    }
}
