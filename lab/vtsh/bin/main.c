#include <proc-clone3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vtsh.h>

#define MAX_INPUT 1024
#define MAX_ARGS 64
#define MAX_BG_PIDS 128
#define COMMAND_NOT_FOUND 127
#define STACK_SIZE 16384

typedef struct {
  pid_t bg_pids[MAX_BG_PIDS];
  int bg_count;
} bg_processes_t;

static void trim_spaces(char* text) {
  if (!text) {
    return;
  }

  while (*text == ' ' || *text == '\t') {
    memmove(text, text + 1, strlen(text));
  }

  int length = (int)strlen(text);
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\n' ||
                        text[length - 1] == '\t' || text[length - 1] == '\r')) {
    text[length - 1] = '\0';
    length--;
  }
}

static void add_bg_pid(bg_processes_t* bg_processes, const pid_t pidd) {
  if (bg_processes->bg_count < MAX_BG_PIDS) {
    bg_processes->bg_pids[bg_processes->bg_count++] = pidd;
  }
}

static bool is_bg_pid(const bg_processes_t* bg_processes, const pid_t piddd) {
  for (int i = 0; i < bg_processes->bg_count; i++) {
    if (bg_processes->bg_pids[i] == piddd) {
      return true;
    }
  }
  return false;
}

static void parse_arguments(char* input, char** args) {
  int count = 0;
  char* saveptr = NULL;
  char* part = strtok_r(input, " \t\n", &saveptr);

  while (part != NULL && count < MAX_ARGS - 1) {
    args[count++] = part;
    part = strtok_r(NULL, " \t\n", &saveptr);
  }
  args[count] = NULL;
}

static void run_cat() {
  char buffer[MAX_INPUT];
  while (fgets(buffer, sizeof(buffer), stdin)) {
    write(STDOUT_FILENO, buffer, strlen(buffer));
  }
  _exit(0);
}

static void run_command(
    bg_processes_t* bg_processes, char** args, bool background
) {
  pid_t pid = do_clone3_or_fork();
  if (pid == -1) {
    perror("clone3/fork");
    return;
  }

  if (pid == 0) {  // Дочерний процесс
    if (args[0] != NULL && strcmp(args[0], "cat") == 0) {
      run_cat();
    } else if (execvp(args[0], args) == -1) {
      perror("execvp");
      _exit(COMMAND_NOT_FOUND);
    }
    _exit(0);
  }

  pid_t child_pid = pid;

  if (background) {
    add_bg_pid(bg_processes, child_pid);
    return;
  }

  int status = 0;
  pid_t wait_pid = waitpid(child_pid, &status, 0);
  if (wait_pid == -1) {
    perror("waitpid");
    return;
  }

  // Обрабатываем результат завершения дочернего процесса
  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status) == COMMAND_NOT_FOUND) {
      printf("Command not found\n");
    }
  }
}

static bool check_ampersand(const char* input) {
  size_t orig_len = strlen(input);
  for (size_t k = orig_len; k > 0; k--) {
    if (input[k - 1] == ' ' || input[k - 1] == '\t') {
      continue;
    }
    if (input[k - 1] == '&') {
      return true;
    }
    break;
  }
  return false;
}

int main(void) {
  bg_processes_t bg_processes = {{0}, 0};
  char input[MAX_INPUT];

  while (true) {
    printf("%s", vtsh_prompt());
    if (fgets(input, sizeof(input), stdin) == NULL) {
      printf("\n");
      break;
    }
    trim_spaces(input);
    if (input[0] == '\0') {
      continue;
    }
    if (strcmp(input, "exit") == 0) {
      break;
    }

    bool background = check_ampersand(input);
    if (background) {
      input[strlen(input) - 1] = '\0';  // Убираем амперсанд
    }

    char* args[MAX_ARGS];
    parse_arguments(input, args);
    if (args[0] != NULL) {
      run_command(&bg_processes, args, background);
    }
  }

  return 0;
}