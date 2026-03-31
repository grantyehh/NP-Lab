#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

int main() {
    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    while (1) {
        printf("mysh> ");
        fflush(stdout);

        if (fgets(line, MAX_LINE, stdin) == NULL) {
            printf("\n");
            break; // Ctrl+D
        }

        line[strcspn(line, "\n")] = 0;

        if (strcmp(line, "") == 0) continue;

        // tokenize
        int argc = 0;
        char *token = strtok(line, " ");
        while (token != NULL && argc < MAX_ARGS - 1) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }
        argv[argc] = NULL;

        // built-in: exit
        if (strcmp(argv[0], "exit") == 0) {
            break;
        }
        // built-in: cd
        if (strcmp(argv[0], "cd") == 0) {
            if (argv[1] == NULL) {
                // cd with no args -> go to HOME
                char *home = getenv("HOME");
                if (home == NULL) {
                    fprintf(stderr, "cd: HOME not set\n");
                } else {
                    if (chdir(home) != 0) {
                        perror("cd");
                    }
                }
            } else {
                if (chdir(argv[1]) != 0) {
                    perror("cd");
                }
            }
            continue;
        }

        // check for single pipe
        int pipe_pos = -1;
        for (int i = 0; i < argc; ++i) {
            if (strcmp(argv[i], "|") == 0) {
                pipe_pos = i;
                break;
            }
        }
        if (pipe_pos == -1) {
            // No pipe: normal execution
            // I/O redirection parsing
            char *infile = NULL;
            char *outfile = NULL;
            int append = 0;
            int new_argc = 0;
            for (int i = 0; i < argc; ++i) {
                if (strcmp(argv[i], "<") == 0 && i + 1 < argc) {
                    infile = argv[i + 1];
                    i++; // skip next
                } else if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
                    outfile = argv[i + 1];
                    append = 0;
                    i++; // skip next
                } else if (strcmp(argv[i], ">>") == 0 && i + 1 < argc) {
                    outfile = argv[i + 1];
                    append = 1;
                    i++; // skip next
                } else {
                    argv[new_argc++] = argv[i];
                }
            }
            argv[new_argc] = NULL;

            pid_t pid = fork();
            if (pid == 0) {
                // child: handle I/O redirection
                if (infile) {
                    int fdin = open(infile, O_RDONLY);
                    if (fdin < 0) {
                        perror("open input file");
                        exit(1);
                    }
                    dup2(fdin, STDIN_FILENO);
                    close(fdin);
                }
                if (outfile) {
                    int fdout;
                    if (append) {
                        fdout = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    } else {
                        fdout = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    }
                    if (fdout < 0) {
                        perror("open output file");
                        exit(1);
                    }
                    dup2(fdout, STDOUT_FILENO);
                    close(fdout);
                }
                execvp(argv[0], argv);
                perror("execvp");
                exit(1);
            } else if (pid > 0) {
                wait(NULL);
            } else {
                perror("fork");
            }
        } else {
            // Support single pipe: split commands
            argv[pipe_pos] = NULL;
            char *left_argv[MAX_ARGS];
            char *right_argv[MAX_ARGS];
            int left_argc = 0, right_argc = 0;
            // left command
            for (int i = 0; i < pipe_pos; ++i) {
                left_argv[left_argc++] = argv[i];
            }
            left_argv[left_argc] = NULL;
            // right command
            for (int i = pipe_pos + 1; i < argc; ++i) {
                right_argv[right_argc++] = argv[i];
            }
            right_argv[right_argc] = NULL;

            // parse I/O redirection for left and right commands
            // left: only care about input redirection
            char *left_infile = NULL;
            char *left_outfile = NULL;
            int left_append = 0;
            char *tmp_argv[MAX_ARGS];
            int tmp_argc = 0;
            for (int i = 0; i < left_argc; ++i) {
                if (strcmp(left_argv[i], "<") == 0 && i + 1 < left_argc) {
                    left_infile = left_argv[i + 1];
                    i++;
                } else if (strcmp(left_argv[i], ">") == 0 && i + 1 < left_argc) {
                    left_outfile = left_argv[i + 1];
                    left_append = 0;
                    i++;
                } else if (strcmp(left_argv[i], ">>") == 0 && i + 1 < left_argc) {
                    left_outfile = left_argv[i + 1];
                    left_append = 1;
                    i++;
                } else {
                    tmp_argv[tmp_argc++] = left_argv[i];
                }
            }
            tmp_argv[tmp_argc] = NULL;
            memcpy(left_argv, tmp_argv, sizeof(char*) * (tmp_argc+1));
            left_argc = tmp_argc;
            // right: only care about output redirection
            char *right_infile = NULL;
            char *right_outfile = NULL;
            int right_append = 0;
            tmp_argc = 0;
            for (int i = 0; i < right_argc; ++i) {
                if (strcmp(right_argv[i], "<") == 0 && i + 1 < right_argc) {
                    right_infile = right_argv[i + 1];
                    i++;
                } else if (strcmp(right_argv[i], ">") == 0 && i + 1 < right_argc) {
                    right_outfile = right_argv[i + 1];
                    right_append = 0;
                    i++;
                } else if (strcmp(right_argv[i], ">>") == 0 && i + 1 < right_argc) {
                    right_outfile = right_argv[i + 1];
                    right_append = 1;
                    i++;
                } else {
                    tmp_argv[tmp_argc++] = right_argv[i];
                }
            }
            tmp_argv[tmp_argc] = NULL;
            memcpy(right_argv, tmp_argv, sizeof(char*) * (tmp_argc+1));
            right_argc = tmp_argc;

            int pipefd[2];
            if (pipe(pipefd) < 0) {
                perror("pipe");
                continue;
            }
            pid_t pid1 = fork();
            if (pid1 == 0) {
                // left child
                close(pipefd[0]); // close read end
                if (left_infile) {
                    int fdin = open(left_infile, O_RDONLY);
                    if (fdin < 0) {
                        perror("open input file");
                        exit(1);
                    }
                    dup2(fdin, STDIN_FILENO);
                    close(fdin);
                }
                if (left_outfile) {
                    int fdout;
                    if (left_append) {
                        fdout = open(left_outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    } else {
                        fdout = open(left_outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    }
                    if (fdout < 0) {
                        perror("open output file");
                        exit(1);
                    }
                    dup2(fdout, STDOUT_FILENO);
                    close(fdout);
                } else {
                    dup2(pipefd[1], STDOUT_FILENO);
                }
                close(pipefd[1]);
                execvp(left_argv[0], left_argv);
                perror("execvp (left)");
                exit(1);
            }
            pid_t pid2 = fork();
            if (pid2 == 0) {
                // right child
                close(pipefd[1]); // close write end
                if (right_outfile) {
                    int fdout;
                    if (right_append) {
                        fdout = open(right_outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    } else {
                        fdout = open(right_outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    }
                    if (fdout < 0) {
                        perror("open output file");
                        exit(1);
                    }
                    dup2(fdout, STDOUT_FILENO);
                    close(fdout);
                }
                if (right_infile) {
                    int fdin = open(right_infile, O_RDONLY);
                    if (fdin < 0) {
                        perror("open input file");
                        exit(1);
                    }
                    dup2(fdin, STDIN_FILENO);
                    close(fdin);
                } else {
                    dup2(pipefd[0], STDIN_FILENO);
                }
                close(pipefd[0]);
                execvp(right_argv[0], right_argv);
                perror("execvp (right)");
                exit(1);
            }
            // parent
            close(pipefd[0]);
            close(pipefd[1]);
            waitpid(pid1, NULL, 0);
            waitpid(pid2, NULL, 0);
        }
    }

    return 0;
}   