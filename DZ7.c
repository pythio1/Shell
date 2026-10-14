#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#define MAX_LINE_LEN 10240
#define MAX_TOKENS 2560

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_REDIR_IN,
    TOKEN_REDIR_OUT,
    TOKEN_REDIR_OUT_APPEND,
    TOKEN_REDIR_ERR
} TokenType;

typedef struct {
    TokenType type;
    char *value;
} Token;

typedef struct {
    char *cmd;
    char **args;
    int argc;
    char *in_file;
    char *out_file;
    int append_out;
    char *err_file;
} Command;

/* Вспомогательная функция для очистки команд при ошибке */
static void cleanup_on_error(Command *cmds, int cmd_count, Command *current) {
    if (current && current->args) {
        for (int j = 0; j < current->argc; j++) {
            free(current->args[j]);
        }
        free(current->args);
        if (current->in_file) free(current->in_file);
        if (current->out_file) free(current->out_file);
        if (current->err_file) free(current->err_file);
        if (current->cmd) free(current->cmd);
    }
    if (cmds) {
        for (int k = 0; k < cmd_count; k++) {
            if (cmds[k].args) {
                for (int j = 0; j < cmds[k].argc; j++) {
                    free(cmds[k].args[j]);
                }
                free(cmds[k].args);
            }
            if (cmds[k].in_file) free(cmds[k].in_file);
            if (cmds[k].out_file) free(cmds[k].out_file);
            if (cmds[k].err_file) free(cmds[k].err_file);
            if (cmds[k].cmd) free(cmds[k].cmd);
        }
        free(cmds);
    }
}

int tokenize_line(const char *line, Token **tokens, int *num_tokens) {
    Token *toks = malloc(MAX_TOKENS * sizeof(Token));
    if (!toks) return 1;
    
    int count = 0;
    int i = 0;
    
    while (line[i] != '\0' && line[i] != '\n') {
        while (isspace((unsigned char)line[i])) i++;
        if (line[i] == '\0' || line[i] == '\n') break;
        
        // Обработка пайпа
        if (line[i] == '|') {
            if (line[i+1] == '|') {
                for (int j = 0; j < count; j++)
                    if (toks[j].value) free(toks[j].value);
                free(toks);
                return 1;
            }
            toks[count].type = TOKEN_PIPE;
            toks[count].value = NULL;
            count++;
            i++;
            continue;
        }
        
        // Обработка перенаправления ввода
        if (line[i] == '<') {
            toks[count].type = TOKEN_REDIR_IN;
            toks[count].value = NULL;
            count++;
            i++;
            continue;
        }
        
        // Обработка перенаправления вывода
        if (line[i] == '>') {
            if (line[i+1] == '>') {
                toks[count].type = TOKEN_REDIR_OUT_APPEND;
                toks[count].value = NULL;
                count++;
                i += 2;
            } else {
                toks[count].type = TOKEN_REDIR_OUT;
                toks[count].value = NULL;
                count++;
                i++;
            }
            continue;
        }
        
        // Обработка перенаправления stderr "2>" - только если это отдельный токен
        if (line[i] == '2' && line[i+1] == '>' && (i == 0 || isspace((unsigned char)line[i-1]))) {
            toks[count].type = TOKEN_REDIR_ERR;
            toks[count].value = NULL;
            count++;
            i += 2;
            continue;
        }
        
        // Обработка кавычек
        if (line[i] == '\"' || line[i] == '\'') {
            char quote = line[i];
            i++;
            int start = i;
            while (line[i] != quote && line[i] != '\0' && line[i] != '\n')
                i++;
            if (line[i] != quote) {
                for (int j = 0; j < count; j++)
                    if (toks[j].value) free(toks[j].value);
                free(toks);
                return 1;
            }
            int len = i - start;
            char *word = malloc(len + 1);
            if (!word) {
                for (int j = 0; j < count; j++)
                    if (toks[j].value) free(toks[j].value);
                free(toks);
                return 1;
            }
            strncpy(word, line + start, len);
            word[len] = '\0';
            toks[count].type = TOKEN_WORD;
            toks[count].value = word;
            count++;
            i++;
            continue;
        }
        
        // Обычное слово
        int start = i;
        while (line[i] != '\0' && line[i] != '\n' && !isspace((unsigned char)line[i]) &&
               line[i] != '|' && line[i] != '<' && line[i] != '>') {
            i++;
        }
        int len = i - start;
        char *word = malloc(len + 1);
        if (!word) {
            for (int j = 0; j < count; j++)
                if (toks[j].value) free(toks[j].value);
            free(toks);
            return 1;
        }
        strncpy(word, line + start, len);
        word[len] = '\0';
        toks[count].type = TOKEN_WORD;
        toks[count].value = word;
        count++;
    }
    
    *tokens = toks;
    *num_tokens = count;
    return 0;
}

void free_tokens(Token *tokens, int num_tokens) {
    if (!tokens) return;
    for (int i = 0; i < num_tokens; i++) {
        if (tokens[i].value) free(tokens[i].value);
    }
    free(tokens);
}

int parse_pipeline(Token *tokens, int num_tokens, Command **commands, int *num_commands) {
    Command *cmds = malloc(sizeof(Command) * (num_tokens + 1));
    if (!cmds) return 1;
    
    int cmd_count = 0;
    Command current;
    memset(&current, 0, sizeof(Command));
    current.args = malloc(sizeof(char*) * (num_tokens + 1));
    if (!current.args) {
        free(cmds);
        return 1;
    }
    
    int redir_phase = 0;
    int i = 0;
    
    while (i < num_tokens) {
        Token tok = tokens[i];
        
        if (tok.type == TOKEN_PIPE) {
            if (current.argc == 0) {
                cleanup_on_error(cmds, cmd_count, &current);
                return 1;
            }
            current.args[current.argc] = NULL;
            if (current.cmd == NULL && current.argc > 0)
                current.cmd = strdup(current.args[0]);
            cmds[cmd_count++] = current;
            
            memset(&current, 0, sizeof(Command));
            current.args = malloc(sizeof(char*) * (num_tokens + 1));
            if (!current.args) {
                cleanup_on_error(cmds, cmd_count, NULL);
                return 1;
            }
            redir_phase = 0;
            i++;
            continue;
        }
        
        if (tok.type == TOKEN_REDIR_IN || tok.type == TOKEN_REDIR_OUT ||
            tok.type == TOKEN_REDIR_OUT_APPEND || tok.type == TOKEN_REDIR_ERR) {
            
            redir_phase = 1;
            
            if (i + 1 >= num_tokens || tokens[i+1].type != TOKEN_WORD) {
                cleanup_on_error(cmds, cmd_count, &current);
                return 1;
            }
            
            char *filename = strdup(tokens[i+1].value);
            if (!filename) {
                cleanup_on_error(cmds, cmd_count, &current);
                return 1;
            }
            
            if (tok.type == TOKEN_REDIR_IN) {
                if (current.in_file != NULL) {
                    free(filename);
                    cleanup_on_error(cmds, cmd_count, &current);
                    return 1;
                }
                current.in_file = filename;
            } else if (tok.type == TOKEN_REDIR_OUT) {
                if (current.out_file != NULL) {
                    free(filename);
                    cleanup_on_error(cmds, cmd_count, &current);
                    return 1;
                }
                current.out_file = filename;
                current.append_out = 0;
            } else if (tok.type == TOKEN_REDIR_OUT_APPEND) {
                if (current.out_file != NULL) {
                    free(filename);
                    cleanup_on_error(cmds, cmd_count, &current);
                    return 1;
                }
                current.out_file = filename;
                current.append_out = 1;
            } else if (tok.type == TOKEN_REDIR_ERR) {
                if (current.err_file != NULL) {
                    free(filename);
                    cleanup_on_error(cmds, cmd_count, &current);
                    return 1;
                }
                current.err_file = filename;
            }
            i += 2;
            continue;
        }
        
        if (tok.type == TOKEN_WORD) {
            if (redir_phase) {
                // После перенаправлений слова не допускаются
                cleanup_on_error(cmds, cmd_count, &current);
                return 1;
            }
            current.args[current.argc] = strdup(tok.value);
            if (!current.args[current.argc]) {
                cleanup_on_error(cmds, cmd_count, &current);
                return 1;
            }
            current.argc++;
            i++;
            continue;
        }
        
        i++;
    }
    
    if (current.argc == 0) {
        cleanup_on_error(cmds, cmd_count, &current);
        return 1;
    }
    
    current.args[current.argc] = NULL;
    if (current.cmd == NULL && current.argc > 0)
        current.cmd = strdup(current.args[0]);
    cmds[cmd_count++] = current;
    
    *commands = cmds;
    *num_commands = cmd_count;
    return 0;
}

void free_commands(Command *commands, int num_commands) {
    if (!commands) return;
    for (int i = 0; i < num_commands; i++) {
        if (commands[i].args) {
            for (int j = 0; j < commands[i].argc; j++) {
                free(commands[i].args[j]);
            }
            free(commands[i].args);
        }
        if (commands[i].cmd) free(commands[i].cmd);
        if (commands[i].in_file) free(commands[i].in_file);
        if (commands[i].out_file) free(commands[i].out_file);
        if (commands[i].err_file) free(commands[i].err_file);
    }
    free(commands);
}

int command_exists(const char *cmd) {
    if (strchr(cmd, '/') != NULL) {
        return access(cmd, X_OK) == 0;
    }
    
    char *path = getenv("PATH");
    if (!path) return 0;
    
    char *path_copy = strdup(path);
    if (!path_copy) return 0;
    
    int found = 0;
    char *token = strtok(path_copy, ":");
    while (token != NULL) {
        char fullpath[PATH_MAX];
        snprintf(fullpath, PATH_MAX, "%s/%s", token, cmd);
        if (access(fullpath, X_OK) == 0) {
            found = 1;
            break;
        }
        token = strtok(NULL, ":");
    }
    free(path_copy);
    return found;
}

/* Проверка: является ли команда встроенной */
static int is_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0 || strcmp(cmd, "exit") == 0;
}

/* Выполнение встроенной команды */
static void run_builtin(Command *cmd) {
    if (strcmp(cmd->cmd, "cd") == 0) {
        if (cmd->argc < 2) {
            char *home = getenv("HOME");
            if (home == NULL) home = "/";
            if (chdir(home) != 0)
                fprintf(stderr, "Directory %s not found\n", home);
        } else {
            if (chdir(cmd->args[1]) != 0)
                fprintf(stderr, "Directory %s not found\n", cmd->args[1]);
        }
    } else if (strcmp(cmd->cmd, "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
            printf("%s\n", cwd);
    }
}

int main(void) {
    char line[MAX_LINE_LEN];
    
    while (1) {
        if (fgets(line, MAX_LINE_LEN, stdin) == NULL) {
            break;
        }
        
        // Пропускаем пустые строки
        int only_space = 1;
        for (int i = 0; line[i] != '\0' && line[i] != '\n'; i++) {
            if (!isspace((unsigned char)line[i])) {
                only_space = 0;
                break;
            }
        }
        if (only_space) continue;
        
        Token *tokens = NULL;
        int num_tokens = 0;
        if (tokenize_line(line, &tokens, &num_tokens) != 0) {
            fprintf(stderr, "Syntax error\n");
            continue;
        }
        
        if (num_tokens == 0) {
            free_tokens(tokens, num_tokens);
            continue;
        }
        
        Command *commands = NULL;
        int num_commands = 0;
        if (parse_pipeline(tokens, num_tokens, &commands, &num_commands) != 0) {
            fprintf(stderr, "Syntax error\n");
            free_tokens(tokens, num_tokens);
            continue;
        }
        free_tokens(tokens, num_tokens);
        
        // Проверка на exit (только для одиночной команды)
        if (num_commands == 1 && strcmp(commands[0].cmd, "exit") == 0) {
            printf("Shell is closing, bye\n");
            free_commands(commands, num_commands);
            exit(0);
        }
        
        // Встроенные команды в родительском процессе (только одиночные)
        if (num_commands == 1 && 
            (strcmp(commands[0].cmd, "cd") == 0 || strcmp(commands[0].cmd, "pwd") == 0)) {
            run_builtin(&commands[0]);
            free_commands(commands, num_commands);
            continue;
        }
        
        // Проверка существования внешних команд
        int invalid_command = 0;
        for (int i = 0; i < num_commands; i++) {
            if (is_builtin(commands[i].cmd)) continue;
            if (!command_exists(commands[i].cmd)) {
                fprintf(stderr, "Command %s not found\n", commands[i].cmd);
                invalid_command = 1;
                break;
            }
        }
        if (invalid_command) {
            free_commands(commands, num_commands);
            continue;
        }
        
        // Проверка перенаправлений
        int redir_error = 0;
        for (int i = 0; i < num_commands && !redir_error; i++) {
            if (commands[i].in_file) {
                int fd = open(commands[i].in_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "File %s not found\n", commands[i].in_file);
                    redir_error = 1;
                } else {
                    close(fd);
                }
            }
            if (!redir_error && commands[i].out_file) {
                int flags = O_WRONLY | O_CREAT;
                flags |= commands[i].append_out ? O_APPEND : O_TRUNC;
                int fd = open(commands[i].out_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "File %s not found\n", commands[i].out_file);
                    redir_error = 1;
                } else {
                    close(fd);
                }
            }
            if (!redir_error && commands[i].err_file) {
                int fd = open(commands[i].err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "File %s not found\n", commands[i].err_file);
                    redir_error = 1;
                } else {
                    close(fd);
                }
            }
        }
        if (redir_error) {
            free_commands(commands, num_commands);
            continue;
        }
        
        // Создание пайпов
        int num_pipes = num_commands - 1;
        int *pipefds = NULL;
        if (num_pipes > 0) {
            pipefds = malloc(2 * num_pipes * sizeof(int));
            if (!pipefds) {
                free_commands(commands, num_commands);
                continue;
            }
            for (int i = 0; i < num_pipes; i++) {
                if (pipe(pipefds + i * 2) < 0) {
                    perror("pipe");
                    free(pipefds);
                    free_commands(commands, num_commands);
                    exit(1);
                }
            }
        }
        
        pid_t *pids = malloc(num_commands * sizeof(pid_t));
        if (!pids) {
            if (pipefds) {
                for (int i = 0; i < 2 * num_pipes; i++) close(pipefds[i]);
                free(pipefds);
            }
            free_commands(commands, num_commands);
            continue;
        }
        
        for (int i = 0; i < num_commands; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                exit(1);
            }
            if (pid == 0) {
                // Настройка stdin
                if (i > 0 && commands[i].in_file == NULL) {
                    dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
                }
                // Настройка stdout
                if (i < num_commands - 1 && commands[i].out_file == NULL) {
                    dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
                }
                
                // Перенаправления из файлов
                if (commands[i].in_file) {
                    int fd = open(commands[i].in_file, O_RDONLY);
                    if (fd < 0) {
                        fprintf(stderr, "File %s not found\n", commands[i].in_file);
                        exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                if (commands[i].out_file) {
                    int flags = O_WRONLY | O_CREAT;
                    flags |= commands[i].append_out ? O_APPEND : O_TRUNC;
                    int fd = open(commands[i].out_file, flags, 0644);
                    if (fd < 0) {
                        fprintf(stderr, "File %s not found\n", commands[i].out_file);
                        exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
                if (commands[i].err_file) {
                    int fd = open(commands[i].err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd < 0) {
                        fprintf(stderr, "File %s not found\n", commands[i].err_file);
                        exit(1);
                    }
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }
                
                // Закрываем дескрипторы пайпов
                if (pipefds) {
                    for (int j = 0; j < 2 * num_pipes; j++)
                        close(pipefds[j]);
                }
                
                // Встроенные команды в дочернем процессе
                if (strcmp(commands[i].cmd, "cd") == 0 || 
                    strcmp(commands[i].cmd, "pwd") == 0) {
                    run_builtin(&commands[i]);
                    exit(0);
                }
                
                // Внешняя команда
                execvp(commands[i].cmd, commands[i].args);
                fprintf(stderr, "Command %s not found\n", commands[i].cmd);
                exit(1);
            } else {
                pids[i] = pid;
            }
        }
        
        // Закрываем пайпы в родителе
        if (pipefds) {
            for (int i = 0; i < 2 * num_pipes; i++)
                close(pipefds[i]);
            free(pipefds);
        }
        
        // Ожидаем завершения
        for (int i = 0; i < num_commands; i++) {
            waitpid(pids[i], NULL, 0);
        }
        free(pids);
        free_commands(commands, num_commands);
    }
    
    return 0;
}
