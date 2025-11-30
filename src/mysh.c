#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* -------------------- Config -------------------- */

#define MAX_TOKENS 256
#define MAX_ARGS 128
#define MAX_HISTORY 1000
#define HISTORY_FILE ".mysh_history"
#define MAX_JOBS 128
#define PROMPT_BUF 1024
#define MAX_ALIASES 100
#define MAX_VARS 100
#define MAX_SUGGESTIONS 10

/* ---------- Cyberpunk Colors ---------- */

#define CLR_RESET       "\033[0m"
#define CLR_NEON_PINK   "\033[38;5;201m"
#define CLR_NEON_BLUE   "\033[38;5;45m"
#define CLR_NEON_GREEN  "\033[38;5;46m"
#define CLR_NEON_PURPLE "\033[38;5;93m"
#define CLR_NEON_CYAN   "\033[38;5;51m"
#define CLR_NEON_YELLOW "\033[38;5;226m"
#define CLR_NEON_ORANGE "\033[38;5;208m"
#define CLR_DARK_GRAY   "\033[38;5;238m"
#define CLR_MID_GRAY    "\033[38;5;245m"
#define CLR_LIGHT_GRAY  "\033[38;5;252m"

/* Background colors */
#define BG_DARK_BLUE    "\033[48;5;17m"
#define BG_DARK_PURPLE  "\033[48;5;53m"

/* Bold colors */
#define BOLD_NEON_PINK  "\033[1;38;5;201m"
#define BOLD_NEON_BLUE  "\033[1;38;5;45m"
#define BOLD_NEON_GREEN "\033[1;38;5;46m"
#define BOLD_NEON_CYAN  "\033[1;38;5;51m"

/* -------------------- Types & Globals -------------------- */

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct {
    int id;
    pid_t pgid;
    char *cmdline;
    job_state_t state;
} job_t;

/* Jobs */
static job_t jobs[MAX_JOBS];
static int jobs_count = 0;
static int next_job_id = 1;

/* History */
static char *history[MAX_HISTORY];
static int history_count = 0;

/* Terminal & foreground tracking */
static struct termios shell_tmodes;
static pid_t shell_pgid;
static pid_t fg_pgid = 0;

/* Aliases */
typedef struct {
    char *name;
    char *value;
} alias_t;

static alias_t aliases[MAX_ALIASES];
static int alias_count = 0;

/* Shell variables */
typedef struct {
    char *name;
    char *value;
} shell_var_t;

static shell_var_t shell_vars[MAX_VARS];
static int var_count = 0;

/* Mini achievements (UI-only) */
typedef struct {
    char *name;
    char *description;
    bool unlocked;
} achievement_t;

static achievement_t achievements[] = {
    {"FIRST_COMMAND", "Execute your first command", false},
    {"CYBER_EXPLORER", "Use TAB completion 10 times", false},
    {"PIPE_MASTER", "Use pipes in commands", false},
    {"BACKGROUND_OPERATOR", "Run 5 background jobs", false},
    {"ALIAS_CREATOR", "Create your first alias", false},
    {"NEON_WARRIOR", "Use all cyberpunk features", false},
    {NULL, NULL, false}
};

/* ---------- Forward declarations ---------- */
static bool is_builtin(const char *cmd);
static char *get_history_path(void);

/* ---------- Utility helpers ---------- */

static void sleep_us(long usec) {
    if (usec <= 0) return;
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static __attribute__((unused)) void xperror(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errno) {
        fprintf(stderr, ": %s", strerror(errno));
    }
    fprintf(stderr, "\n");
}

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    char *r = strdup(s);
    if (!r) { perror("strdup"); exit(1); }
    return r;
}

/* ---------- History path ---------- */

static char *get_history_path(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
        else home = ".";
    }
    size_t n = strlen(home) + 1 + strlen(HISTORY_FILE) + 1;
    char *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    snprintf(p, n, "%s/%s", home, HISTORY_FILE);
    return p;
}

/* ---------- Path helpers ---------- */

/* Expand ~ to HOME */
static char *expand_tilde(const char *path) {
    if (!path || path[0] != '~') return strdup_safe(path);

    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/";
    }

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s%s", home, path + 1);
    return strdup_safe(buf);
}

/* Convert WSL style /mnt/x/... to Windows-like drive path (best-effort).
   Added bounds-check to avoid reading past the string. */
static void convert_path_windows(char *dst, const char *src) {
    size_t src_len = src ? strlen(src) : 0;
    if (src_len >= 7 && strncmp(src, "/mnt/", 5) == 0 && src[6] == '/') {
        char drive = toupper(src[5]);
        snprintf(dst, 1024, "%c:\\%s", drive, src+7);
        for (int i = 0; dst[i]; i++)
            if (dst[i] == '/') dst[i] = '\\';
    } else {
        strncpy(dst, src ? src : "", 1024-1);
        dst[1023] = '\0';
    }
}

/* ---------- Environment expansion ---------- */

static char *expand_env_vars(const char *s) {
    char result[4096] = {0};
    int ri = 0;

    for (int i = 0; s && s[i] && ri < 4090; i++) {
        if (s[i] == '$') {
            char var[128];
            int vi = 0;
            i++;
            while (s[i] && (isalnum((unsigned char)s[i]) || s[i]=='_') && vi < 127) {
                var[vi++] = s[i++];
            }
            var[vi] = 0;
            i--;

            const char *val = NULL;
            for (int j = 0; j < var_count; j++) {
                if (strcmp(shell_vars[j].name, var) == 0) {
                    val = shell_vars[j].value;
                    break;
                }
            }
            if (!val) val = getenv(var);
            if (val) {
                for (int k=0; val[k] && ri < 4090; k++)
                    result[ri++] = val[k];
            }
        } else {
            result[ri++] = s[i];
        }
    }
    result[ri] = 0;
    return strdup_safe(result);
}

/* ---------- UI: borders, header, prompt ---------- */

static void print_header_border(const char *title) {
    printf(CLR_NEON_CYAN "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_NEON_CYAN "│" CLR_RESET);
    printf(BOLD_NEON_CYAN " %-63s " CLR_RESET, title);
    printf(CLR_NEON_CYAN "│\n" CLR_RESET);
    printf(CLR_NEON_CYAN "└─────────────────────────────────────────────────────────────────┘\n" CLR_RESET);
}

static void print_section_border(const char *title) {
    printf(CLR_DARK_GRAY "├── " CLR_NEON_CYAN "%s" CLR_DARK_GRAY " ", title);
    for (int i = strlen(title) + 4; i < 65; i++) printf("─");
    printf("┤\n" CLR_RESET);
}

static void print_content_line(const char *left, const char *right) {
    printf(CLR_DARK_GRAY "│ " CLR_NEON_CYAN "%-20s" CLR_DARK_GRAY " " CLR_LIGHT_GRAY "%-42s" CLR_DARK_GRAY " │\n", left, right);
}

static void print_bottom_border() {
    printf(CLR_DARK_GRAY "└─────────────────────────────────────────────────────────────────┘\n" CLR_RESET);
}

/* Loading bar, boot sound and achievement popups (cosmetic) */

static void show_loading_bar(const char *message) {
    printf("\n" CLR_DARK_GRAY "[" CLR_NEON_CYAN "SYSTEM" CLR_DARK_GRAY "] " CLR_NEON_PINK "%s" CLR_RESET "\n", message);
    printf(CLR_DARK_GRAY "[");
    for (int i = 0; i < 20; i++) {
        printf(CLR_NEON_CYAN "█");
        fflush(stdout);
        sleep_us(25000);
    }
    printf(CLR_DARK_GRAY "] " CLR_NEON_GREEN "DONE\n\n" CLR_RESET);
}

static void play_boot_sound() {
    printf("\a");
    sleep_us(200000);
    printf("\a");
}

static void unlock_achievement(const char *name, const char *description) {
    printf("\n");
    printf(CLR_NEON_PINK "╭─────────────────────────────────────────────────────────────────╮\n" CLR_RESET);
    printf(CLR_NEON_PINK "│" CLR_NEON_YELLOW "    🏆 ACHIEVEMENT UNLOCKED! 🏆           " CLR_NEON_PINK "                         │\n" CLR_RESET);
    printf(CLR_NEON_PINK "│" BOLD_NEON_CYAN " %-63s " CLR_NEON_PINK "│\n", name);
    printf(CLR_NEON_PINK "│" CLR_NEON_GREEN " %-63s " CLR_NEON_PINK "│\n", description);
    printf(CLR_NEON_PINK "╰─────────────────────────────────────────────────────────────────╯\n" CLR_RESET);
    printf("\n");
    for (int i = 0; i < 2; i++) {
        printf("\a");
        sleep_us(150000);
    }
}

static void check_achievements(const char *command, int cmd_count) {
    static int command_count = 0;
    static int bg_count = 0;
    static bool first_command_done = false;

    command_count = cmd_count;

    if (command_count == 1 && !first_command_done && !achievements[0].unlocked) {
        achievements[0].unlocked = true;
        first_command_done = true;
        unlock_achievement("FIRST_COMMAND", "Execute your first command in Cyber-Shell");
    }

    if (strchr(command, '|') && !achievements[2].unlocked) {
        achievements[2].unlocked = true;
        unlock_achievement("PIPE_MASTER", "Use pipeline operations in commands");
    }

    if (strstr(command, "&") && !achievements[3].unlocked) {
        bg_count++;
        if (bg_count >= 3) {
            achievements[3].unlocked = true;
            unlock_achievement("BACKGROUND_OPERATOR", "Run commands in background");
        }
    }
}

static void print_cyberpunk_header() {
    printf("\n");
    print_header_border("🚀 CYBER-SHELL v2.0 🚀");
    printf(CLR_NEON_PURPLE "     Advanced Command Interface • Neural Network Online\n" CLR_RESET);
    printf("\n");

    play_boot_sound();
    show_loading_bar("INITIALIZING NEURAL INTERFACE");

    char host[128]; gethostname(host, sizeof(host));
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "user";

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%H:%M:%S • %Y-%m-%d", &tm);

    printf(CLR_DARK_GRAY "[" CLR_NEON_CYAN "SYSTEM STATUS" CLR_DARK_GRAY "]\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│ " CLR_NEON_CYAN "👤 USER: " CLR_LIGHT_GRAY "%-12s" CLR_NEON_CYAN " 🖥️  HOST: " CLR_LIGHT_GRAY "%-15s" CLR_NEON_CYAN " 🕐 TIME: " CLR_LIGHT_GRAY "%s" CLR_DARK_GRAY " │\n",
           user, host, timestr);
    printf("\n");

    printf(CLR_NEON_PURPLE "💡 " CLR_NEON_CYAN "Type 'help' for cyber-commands • 'exit' to terminate session\n" CLR_RESET);
    printf(CLR_NEON_PURPLE "🔮 " CLR_NEON_CYAN "TAB-completion active • Neural suggestions enabled\n" CLR_RESET);
    printf("\n");
}

/* Build the prompt string */
static void build_cyberpunk_prompt(char *buf, size_t bufsz, int last_status) {
    char host[128]; gethostname(host, sizeof(host));
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "user";

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char timestr[16];
    strftime(timestr, sizeof(timestr), "%H:%M", &tm);

    char cwd[512]; getcwd(cwd, sizeof(cwd));
    char display_cwd[512];
    convert_path_windows(display_cwd, cwd);

    int bgcount = 0;
    for (int i=0;i<jobs_count;i++) if (jobs[i].state==JOB_RUNNING) bgcount++;

    const char *status_icon = (last_status == 0) ? CLR_NEON_GREEN "✓" : CLR_NEON_PINK "✗";
    const char *prompt_char = CLR_NEON_CYAN "➜" CLR_RESET;

    if (bgcount > 0) {
        snprintf(buf, bufsz,
            CLR_DARK_GRAY "[" CLR_RESET "%s" CLR_DARK_GRAY "] " CLR_RESET
            CLR_NEON_PINK "%s" CLR_RESET CLR_DARK_GRAY "@" CLR_RESET
            CLR_NEON_CYAN "%s" CLR_RESET CLR_DARK_GRAY " • " CLR_RESET
            CLR_NEON_YELLOW "%s" CLR_RESET CLR_DARK_GRAY " • " CLR_RESET
            CLR_NEON_BLUE "%s" CLR_RESET " %s " CLR_NEON_ORANGE "[bg:%d]" CLR_RESET " ",
            status_icon, user, host, timestr, display_cwd, prompt_char, bgcount);
    } else {
        snprintf(buf, bufsz,
            CLR_DARK_GRAY "[" CLR_RESET "%s" CLR_DARK_GRAY "] " CLR_RESET
            CLR_NEON_PINK "%s" CLR_RESET CLR_DARK_GRAY "@" CLR_RESET
            CLR_NEON_CYAN "%s" CLR_RESET CLR_DARK_GRAY " • " CLR_RESET
            CLR_NEON_YELLOW "%s" CLR_RESET CLR_DARK_GRAY " • " CLR_RESET
            CLR_NEON_BLUE "%s" CLR_RESET " %s ",
            status_icon, user, host, timestr, display_cwd, prompt_char);
    }
}

/* Error printing */
static void print_cyberpunk_error(const char *text) {
    printf(CLR_DARK_GRAY "[" CLR_NEON_PINK "ERROR" CLR_DARK_GRAY "] " CLR_NEON_PINK "%s\n" CLR_RESET, text);
}

/* Optional pretty output (unused) */
static __attribute__((unused)) void print_cyberpunk_output(const char *text) {
    printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "OUTPUT" CLR_DARK_GRAY "] " CLR_RESET "%s\n", text);
}

/* ---------- Syntax coloring for prompt echo ---------- */

static void print_with_syntax_highlighting(const char *text) {
    if (!text || !*text) return;

    char *copy = strdup_safe(text);
    char *saveptr = NULL;
    char *token = strtok_r(copy, " ", &saveptr);
    int first_token = 1;

    while (token) {
        if (first_token) {
            if (is_builtin(token)) {
                printf(CLR_NEON_GREEN "%s" CLR_RESET, token);
            } else if (access(token, X_OK) == 0) {
                printf(CLR_NEON_CYAN "%s" CLR_RESET, token);
            } else {
                printf(CLR_LIGHT_GRAY "%s" CLR_RESET, token);
            }
            first_token = 0;
        } else if (token[0] == '-') {
            printf(CLR_NEON_YELLOW "%s" CLR_RESET, token);
        } else if (token[0] == '"' || token[0] == '\'') {
            printf(CLR_NEON_BLUE "%s" CLR_RESET, token);
        } else if (token[0] == '$') {
            printf(CLR_NEON_PURPLE "%s" CLR_RESET, token);
        } else if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0 ||
                   strcmp(token, "<") == 0 || strcmp(token, "|") == 0) {
            printf(CLR_NEON_PINK "%s" CLR_RESET, token);
        } else {
            printf(CLR_LIGHT_GRAY "%s" CLR_RESET, token);
        }

        token = strtok_r(NULL, " ", &saveptr);
        if (token) printf(" ");
    }

    free(copy);
}

/* ---------- Alias management ---------- */

static void add_alias(const char *name, const char *value) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].value);
            aliases[i].value = strdup_safe(value);
            return;
        }
    }

    if (alias_count < MAX_ALIASES) {
        aliases[alias_count].name = strdup_safe(name);
        aliases[alias_count].value = strdup_safe(value);
        alias_count++;
    }
}

static char *expand_aliases(const char *input) {
    if (!input || !*input) return strdup_safe(input);

    /* Get first word */
    char *copy = strdup_safe(input);
    char *first_word = strtok(copy, " \t");
    if (!first_word) {
        free(copy);
        return strdup_safe(input);
    }

    for (int i = 0; i < alias_count; i++) {
        if (strcmp(first_word, aliases[i].name) == 0) {
            const char *rest = input + strlen(first_word);
            while (*rest == ' ' || *rest == '\t') rest++;

            char expanded[4096];
            snprintf(expanded, sizeof(expanded), "%s", aliases[i].value);

            if (*rest != '\0') {
                strncat(expanded, " ", sizeof(expanded) - strlen(expanded) - 1);
                strncat(expanded, rest, sizeof(expanded) - strlen(expanded) - 1);
            }

            free(copy);
            return strdup_safe(expanded);
        }
    }

    free(copy);
    return strdup_safe(input);
}

/* ---------- Shell variables ---------- */

static void set_shell_var(const char *name, const char *value) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(shell_vars[i].name, name) == 0) {
            free(shell_vars[i].value);
            shell_vars[i].value = strdup_safe(value);
            return;
        }
    }

    if (var_count < MAX_VARS) {
        shell_vars[var_count].name = strdup_safe(name);
        shell_vars[var_count].value = strdup_safe(value);
        var_count++;
    }
}

/* ---------- Persistent config: save/load aliases & vars ---------- */

static void save_persistent_data() {
    char *path = get_history_path();
    /* replace .mysh_history with .mysh_history_config (best-effort) */
    char *dot = strrchr(path, '.');
    if (dot) {
        /* ensure we don't overflow */
        char newpath[4096];
        snprintf(newpath, sizeof(newpath), "%s_config", path);
        free(path);
        path = strdup_safe(newpath);
    } else {
        char newpath[4096];
        snprintf(newpath, sizeof(newpath), "%s_config", path);
        free(path);
        path = strdup_safe(newpath);
    }

    FILE *f = fopen(path, "w");
    if (!f) { free(path); return; }

    for (int i = 0; i < alias_count; i++) {
        fprintf(f, "alias %s=%s\n", aliases[i].name, aliases[i].value);
    }

    for (int i = 0; i < var_count; i++) {
        fprintf(f, "set %s=%s\n", shell_vars[i].name, shell_vars[i].value);
    }

    fclose(f);
    free(path);
}

static void load_persistent_data() {
    char *path = get_history_path();
    char *dot = strrchr(path, '.');
    if (dot) {
        char newpath[4096];
        snprintf(newpath, sizeof(newpath), "%s_config", path);
        free(path);
        path = strdup_safe(newpath);
    } else {
        char newpath[4096];
        snprintf(newpath, sizeof(newpath), "%s_config", path);
        free(path);
        path = strdup_safe(newpath);
    }

    FILE *f = fopen(path, "r");
    if (!f) { free(path); return; }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    while ((read = getline(&line, &len, f)) != -1) {
        if (read > 0 && (line[read-1] == '\n' || line[read-1] == '\r')) {
            line[--read] = 0;
        }

        if (strncmp(line, "alias ", 6) == 0) {
            char *eq = strchr(line + 6, '=');
            if (eq) {
                *eq = 0;
                add_alias(line + 6, eq + 1);
            }
        } else if (strncmp(line, "set ", 4) == 0) {
            char *eq = strchr(line + 4, '=');
            if (eq) {
                *eq = 0;
                set_shell_var(line + 4, eq + 1);
            }
        }
    }

    free(line);
    fclose(f);
    free(path);
}

/* ---------- Jobs management ---------- */

static void add_job(pid_t pgid, char *cmdline, job_state_t state) {
    if (jobs_count >= MAX_JOBS) return;
    job_t *j = &jobs[jobs_count++];
    j->id = next_job_id++;
    j->pgid = pgid;
    j->cmdline = strdup_safe(cmdline);
    j->state = state;
}

static job_t* find_job_by_pgid(pid_t pgid) {
    for (int i=0;i<jobs_count;i++) if (jobs[i].pgid == pgid) return &jobs[i];
    return NULL;
}

static job_t* find_job_by_id(int id) {
    for (int i=0;i<jobs_count;i++) if (jobs[i].id == id) return &jobs[i];
    return NULL;
}

static void remove_done_jobs() {
    int w = 0;
    for (int i=0;i<jobs_count;i++) {
        if (jobs[i].state == JOB_DONE) {
            free(jobs[i].cmdline);
            continue;
        }
        if (w != i) jobs[w] = jobs[i];
        w++;
    }
    jobs_count = w;
}

static void print_jobs() {
    printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                    BACKGROUND PROCESSES                    " CLR_DARK_GRAY "│\n" CLR_RESET);
    printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

    for (int i=0;i<jobs_count;i++) {
        const char *s = jobs[i].state==JOB_RUNNING? CLR_NEON_GREEN "Running" :
                       jobs[i].state==JOB_STOPPED? CLR_NEON_YELLOW "Stopped" :
                       CLR_NEON_PINK "Done";
        printf(CLR_DARK_GRAY "│ " CLR_NEON_CYAN "[%d]" CLR_DARK_GRAY " %-10s " CLR_LIGHT_GRAY "%-47s" CLR_DARK_GRAY " │\n",
               jobs[i].id, s, jobs[i].cmdline);
    }

    printf(CLR_DARK_GRAY "└─────────────────────────────────────────────────────────────────┘\n" CLR_RESET);
}

/* ---------- History ---------- */

static void load_history() {
    char *path = get_history_path();
    FILE *f = fopen(path, "r");
    if (!f) { free(path); return; }
    char *line = NULL;
    size_t len=0;
    ssize_t r;
    while ((r=getline(&line,&len,f))!=-1) {
        if (r>0 && (line[r-1]=='\n' || line[r-1]=='\r')) line[--r]=0;
        if (history_count < MAX_HISTORY) history[history_count++] = strdup_safe(line);
    }
    free(line);
    fclose(f);
    free(path);
}

static void save_history() {
    char *path = get_history_path();
    FILE *f = fopen(path, "w");
    if (!f) { free(path); return; }
    int start = history_count > MAX_HISTORY ? history_count - MAX_HISTORY : 0;
    for (int i=start;i<history_count;i++) {
        fprintf(f, "%s\n", history[i]);
    }
    fclose(f);
    free(path);
}

static void push_history(const char *line) {
    if (!line || !*line) return;
    if (history_count>0 && strcmp(history[history_count-1], line)==0) return;
    if (history_count < MAX_HISTORY) history[history_count++] = strdup_safe(line);
    else {
        free(history[0]);
        memmove(&history[0], &history[1], sizeof(char*)*(MAX_HISTORY-1));
        history[MAX_HISTORY-1] = strdup_safe(line);
    }
}

/* ---------- Signals & handlers ---------- */

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (int i = 0; i < jobs_count; i++) {
            if (jobs[i].pgid > 0) {
                if (pid == jobs[i].pgid || getpgid(pid) == jobs[i].pgid) {
                    if (WIFEXITED(status) || WIFSIGNALED(status)) {
                        jobs[i].state = JOB_DONE;
                        printf(CLR_DARK_GRAY "[" CLR_NEON_PURPLE "JOB COMPLETED" CLR_DARK_GRAY "] "
                               CLR_LIGHT_GRAY "Job [%d] finished\n" CLR_RESET, jobs[i].id);
                    } else if (WIFSTOPPED(status)) {
                        jobs[i].state = JOB_STOPPED;
                    } else if (WIFCONTINUED(status)) {
                        jobs[i].state = JOB_RUNNING;
                    }
                    break;
                }
            }
        }
    }
}

static void setup_signals() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) perror("sigaction(SIGCHLD)");

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}

static void forward_signal_to_fg(int signo) {
    if (fg_pgid > 0) kill(-fg_pgid, signo);
}

static void sigint_handler(int signo) { (void)signo; forward_signal_to_fg(SIGINT); }
static void sigtstp_handler(int signo) { (void)signo; forward_signal_to_fg(SIGTSTP); }

/* ---------- Parsing structures ---------- */

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    bool append;
} cmd_t;

typedef struct {
    cmd_t cmds[16];
    int ncmds;
    bool background;
} pipeline_t;

/* ---------- Tokenizer ---------- */
/* Handles quotes, escapes, and environment expansion. Returns an array of
   dynamically-allocated strings and sets ntoks_out. Caller must free. */

static char **tokenize(const char *line, int *ntoks_out) {
    char **toks = calloc(MAX_TOKENS, sizeof(char*));
    int ti = 0;
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char buf[4096]; int bi = 0;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && quote == '"' && *(p+1)) {
                    p++;
                    if (bi < (int)sizeof(buf)-1) buf[bi++] = *p++;
                } else {
                    if (bi < (int)sizeof(buf)-1) buf[bi++] = *p++;
                }
            }
            if (*p == quote) p++;
        } else {
            while (*p && !isspace((unsigned char)*p)) {
                if (*p == '\\' && *(p+1)) { p++; if (bi < (int)sizeof(buf)-1) buf[bi++] = *p++; }
                else { if (bi < (int)sizeof(buf)-1) buf[bi++] = *p++; }
            }
        }
        buf[bi] = '\0';

        char *expanded = expand_env_vars(buf);
        toks[ti++] = expanded;
        if (ti >= MAX_TOKENS-1) break;
    }
    toks[ti] = NULL;
    *ntoks_out = ti;
    return toks;
}

/* ---------- Parse tokens into pipeline_t ---------- */

static pipeline_t parse_tokens(char **toks, int ntoks) {
    pipeline_t pipe = { .ncmds = 0, .background = false };
    cmd_t cur = { .argc = 0, .infile = NULL, .outfile = NULL, .append = false };
    int i = 0;
    while (i < ntoks) {
        char *t = toks[i];
        if (strcmp(t, "|") == 0) {
            cur.argv[cur.argc] = NULL;
            pipe.cmds[pipe.ncmds++] = cur;
            cur = (cmd_t){ .argc=0, .infile=NULL, .outfile=NULL, .append=false };
            i++;
        } else if (strcmp(t, "<") == 0) {
            if (i+1 < ntoks) { cur.infile = strdup_safe(toks[i+1]); i+=2; }
            else { i++; }
        } else if (strcmp(t, ">") == 0) {
            if (i+1 < ntoks) { cur.outfile = strdup_safe(toks[i+1]); cur.append=false; i+=2; }
            else { i++; }
        } else if (strcmp(t, ">>") == 0) {
            if (i+1 < ntoks) { cur.outfile = strdup_safe(toks[i+1]); cur.append=true; i+=2; }
            else { i++; }
        } else if (strcmp(t, "&") == 0) {
            pipe.background = true; i++;
        } else {
            if (cur.argc < MAX_ARGS-1) cur.argv[cur.argc++] = strdup_safe(t);
            i++;
        }
    }
    if (cur.argc > 0 || cur.infile || cur.outfile) {
        cur.argv[cur.argc] = NULL;
        pipe.cmds[pipe.ncmds++] = cur;
    }
    return pipe;
}

/* Free tokenizer results */
static void free_tokens(char **toks, int ntoks) {
    for (int i=0;i<ntoks;i++) free(toks[i]);
    free(toks);
}

/* ---------- Builtins ---------- */

/* cd with tilde expansion */
static int builtin_cd(int argc, char **argv) {
    const char *path;
    if (argc < 2) path = getenv("HOME");
    else path = argv[1];

    char *expanded = expand_tilde(path);
    if (chdir(expanded) != 0) {
        print_cyberpunk_error("cd: Directory not found");
        free(expanded);
        return 1;
    }
    free(expanded);
    return 0;
}

/* exit (saves state before quitting) */
static int builtin_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    save_history();
    save_persistent_data();

    printf("\n");
    print_header_border("🛑 SESSION TERMINATED 🛑");
    printf(CLR_NEON_GREEN "         Neural interface disconnecting • Goodbye!\n" CLR_RESET);
    printf("\n");

    exit(0);
    return 0;
}

static int builtin_mkdir(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("mkdir: missing operand");
        return 1;
    }
    for (int i=1;i<argc;i++) {
        if (mkdir(argv[i], 0755) != 0) {
            print_cyberpunk_error("mkdir: Failed to create directory");
        } else {
            printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "CREATED" CLR_DARK_GRAY "] " CLR_RESET "Directory: %s\n", argv[i]);
        }
    }
    return 0;
}

static int builtin_touch(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("touch: missing file operand");
        return 1;
    }
    for (int i=1;i<argc;i++) {
        int fd = open(argv[i], O_CREAT|O_WRONLY, 0644);
        if (fd < 0) {
            print_cyberpunk_error("touch: Failed to create file");
        } else {
            close(fd);
            printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "CREATED" CLR_DARK_GRAY "] " CLR_RESET "File: %s\n", argv[i]);
        }
    }
    return 0;
}

static int builtin_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\033[H\033[2J");
    fflush(stdout);
    return 0;
}

static int builtin_help(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n");
    print_header_border("🎮 CYBER-COMMANDS 🎮");
    printf("\n");

    printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│" CLR_NEON_GREEN "                         CORE COMMANDS                          " CLR_DARK_GRAY "│\n" CLR_RESET);
    printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

    print_content_line("cd [dir]", "Navigate directories (cd ~ for home)");
    print_content_line("exit", "Terminate cyber-session");
    print_content_line("mkdir/touch", "Create directories/files");
    print_content_line("clear", "Clear terminal display");
    print_content_line("history", "View command history");
    print_content_line("jobs/fg/bg", "Manage background processes");

    print_section_border("CUSTOMIZATION");
    print_content_line("alias/unalias", "Create/remove command shortcuts");
    print_content_line("set/unset", "Manage shell variables");
    print_content_line("vars/aliases", "List all variables and aliases");

    print_section_border("FEATURES");
    print_content_line("TAB completion", "Auto-complete filenames");
    print_content_line("~ expansion", "Use ~ for home directory");
    print_content_line("$ variables", "Environment and shell variables");
    print_content_line("Pipes & Redirection", "| > >> <");
    print_content_line("Background jobs", "Use & to run in background");

    print_bottom_border();

    printf("\n");
    printf(CLR_NEON_PURPLE "💡 " CLR_NEON_CYAN "Pro tip: Add ? to any command to see tokenized preview\n" CLR_RESET);
    printf("\n");

    return 0;
}

static int builtin_history(int argc, char **argv) {
    (void)argc; (void)argv;

    printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                        COMMAND HISTORY                         " CLR_DARK_GRAY "│\n" CLR_RESET);
    printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

    for (int i=0;i<history_count;i++) {
        printf(CLR_DARK_GRAY "│ " CLR_NEON_PURPLE "%4d" CLR_DARK_GRAY " │ " CLR_LIGHT_GRAY "%-55s" CLR_DARK_GRAY " │\n", i+1, history[i]);
    }

    print_bottom_border();

    return 0;
}

static int builtin_histsearch(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("histsearch <term>");
        return 1;
    }

    printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                        SEARCH RESULTS                          " CLR_DARK_GRAY "│\n" CLR_RESET);
    printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

    int found = 0;
    for (int i=0;i<history_count;i++) {
        if (strstr(history[i], argv[1])) {
            printf(CLR_DARK_GRAY "│ " CLR_NEON_PURPLE "%4d" CLR_DARK_GRAY " │ " CLR_LIGHT_GRAY "%-55s" CLR_DARK_GRAY " │\n", i+1, history[i]);
            found = 1;
        }
    }

    if (!found) {
        printf(CLR_DARK_GRAY "│ " CLR_NEON_PINK " No matches found for: %-40s " CLR_DARK_GRAY "│\n", argv[1]);
    }

    print_bottom_border();

    return 0;
}

static int builtin_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    print_jobs();
    remove_done_jobs();
    return 0;
}

static int builtin_fg(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("fg <jobid>");
        return 1;
    }
    int id = atoi(argv[1]);
    job_t *j = find_job_by_id(id);
    if (!j) {
        print_cyberpunk_error("fg: no such job");
        return 1;
    }
    j->state = JOB_RUNNING;
    tcsetpgrp(STDIN_FILENO, j->pgid);
    fg_pgid = j->pgid;
    if (kill(-j->pgid, SIGCONT) < 0) perror("kill(SIGCONT)");
    int status;
    waitpid(-j->pgid, &status, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    fg_pgid = 0;
    return 0;
}

static int builtin_bg(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("bg <jobid>");
        return 1;
    }
    int id = atoi(argv[1]);
    job_t *j = find_job_by_id(id);
    if (!j) {
        print_cyberpunk_error("bg: no such job");
        return 1;
    }
    j->state = JOB_RUNNING;
    if (kill(-j->pgid, SIGCONT) < 0) perror("kill(SIGCONT)");
    return 0;
}

static int builtin_alias(int argc, char **argv) {
    if (argc == 1) {
        printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
        printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                       COMMAND ALIASES                         " CLR_DARK_GRAY "│\n" CLR_RESET);
        printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

        for (int i = 0; i < alias_count; i++) {
            printf(CLR_DARK_GRAY "│ " CLR_NEON_GREEN "%-20s" CLR_DARK_GRAY " → " CLR_LIGHT_GRAY "%-40s" CLR_DARK_GRAY " │\n",
                   aliases[i].name, aliases[i].value);
        }

        print_bottom_border();
        return 0;
    }

    if (argc >= 3) {
        char value[4096] = {0};
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(value, " ", sizeof(value) - strlen(value) - 1);
            strncat(value, argv[i], sizeof(value) - strlen(value) - 1);
        }
        add_alias(argv[1], value);
        printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "ALIAS CREATED" CLR_DARK_GRAY "] " CLR_NEON_GREEN "%s" CLR_DARK_GRAY " → " CLR_LIGHT_GRAY "%s\n" CLR_RESET,
               argv[1], value);
        return 0;
    }

    print_cyberpunk_error("alias: usage: alias name value");
    return 1;
}

static int builtin_unalias(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("unalias: missing argument");
        return 1;
    }

    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, argv[1]) == 0) {
            free(aliases[i].name);
            free(aliases[i].value);
            for (int j = i; j < alias_count - 1; j++) {
                aliases[j] = aliases[j + 1];
            }
            alias_count--;
            printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "ALIAS REMOVED" CLR_DARK_GRAY "] " CLR_LIGHT_GRAY "%s\n" CLR_RESET, argv[1]);
            return 0;
        }
    }

    print_cyberpunk_error("unalias: not found");
    return 1;
}

static int builtin_set(int argc, char **argv) {
    if (argc < 3) {
        print_cyberpunk_error("set: usage: set <name> <value>");
        return 1;
    }

    set_shell_var(argv[1], argv[2]);
    printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "VARIABLE SET" CLR_DARK_GRAY "] " CLR_NEON_GREEN "%s" CLR_DARK_GRAY " = " CLR_LIGHT_GRAY "%s\n" CLR_RESET, argv[1], argv[2]);
    return 0;
}

static int builtin_unset(int argc, char **argv) {
    if (argc < 2) {
        print_cyberpunk_error("unset: missing argument");
        return 1;
    }

    for (int i = 0; i < var_count; i++) {
        if (strcmp(shell_vars[i].name, argv[1]) == 0) {
            free(shell_vars[i].name);
            free(shell_vars[i].value);
            for (int j = i; j < var_count - 1; j++) {
                shell_vars[j] = shell_vars[j + 1];
            }
            var_count--;
            printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "VARIABLE REMOVED" CLR_DARK_GRAY "] " CLR_LIGHT_GRAY "%s\n" CLR_RESET, argv[1]);
            return 0;
        }
    }

    print_cyberpunk_error("unset: not found");
    return 1;
}

static int builtin_vars(int argc, char **argv) {
    (void)argc; (void)argv;

    printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
    printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                       SHELL VARIABLES                         " CLR_DARK_GRAY "│\n" CLR_RESET);
    printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);

    for (int i = 0; i < var_count; i++) {
        printf(CLR_DARK_GRAY "│ " CLR_NEON_PURPLE "%-20s" CLR_DARK_GRAY " = " CLR_LIGHT_GRAY "%-40s" CLR_DARK_GRAY " │\n",
               shell_vars[i].name, shell_vars[i].value);
    }

    print_bottom_border();

    return 0;
}

static int builtin_aliases(int argc, char **argv) {
    (void)argc;
    return builtin_alias(1, argv);
}

/* Builtin dispatch helper */
static bool is_builtin(const char *cmd) {
    const char *builtins[] = {
        "cd","exit","mkdir","touch","clear","help","history","histsearch",
        "jobs","fg","bg","alias","unalias","set","unset","vars","aliases", NULL
    };
    for (int i=0; builtins[i]; i++) if (strcmp(cmd, builtins[i])==0) return true;
    return false;
}

static int run_builtin(int argc, char **argv) {
    if (argc<=0) return 0;
    if (strcmp(argv[0],"cd")==0) return builtin_cd(argc,argv);
    if (strcmp(argv[0],"exit")==0) return builtin_exit(argc,argv);
    if (strcmp(argv[0],"mkdir")==0) return builtin_mkdir(argc,argv);
    if (strcmp(argv[0],"touch")==0) return builtin_touch(argc,argv);
    if (strcmp(argv[0],"clear")==0) return builtin_clear(argc,argv);
    if (strcmp(argv[0],"help")==0) return builtin_help(argc,argv);
    if (strcmp(argv[0],"history")==0) return builtin_history(argc,argv);
    if (strcmp(argv[0],"histsearch")==0) return builtin_histsearch(argc,argv);
    if (strcmp(argv[0],"jobs")==0) return builtin_jobs(argc,argv);
    if (strcmp(argv[0],"fg")==0) return builtin_fg(argc,argv);
    if (strcmp(argv[0],"bg")==0) return builtin_bg(argc,argv);
    if (strcmp(argv[0],"alias")==0) return builtin_alias(argc,argv);
    if (strcmp(argv[0],"unalias")==0) return builtin_unalias(argc,argv);
    if (strcmp(argv[0],"set")==0) return builtin_set(argc,argv);
    if (strcmp(argv[0],"unset")==0) return builtin_unset(argc,argv);
    if (strcmp(argv[0],"vars")==0) return builtin_vars(argc,argv);
    if (strcmp(argv[0],"aliases")==0) return builtin_aliases(argc,argv);
    return 127;
}

/* ---------- Execution helpers ---------- */

/* Setup IO redirection for a child process. On failure the child exits. */
static void redirect_io(const char *infile, const char *outfile, bool append) {
    if (infile) {
        int fd = open(infile, O_RDONLY);
        if (fd < 0) { perror("open infile"); exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (outfile) {
        int flags = O_WRONLY | O_CREAT | (append? O_APPEND : O_TRUNC);
        int fd = open(outfile, flags, 0644);
        if (fd < 0) { perror("open outfile"); exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

/* ---------- Tab completion helper (simple file-name completion) ---------- */

static char *tab_complete(const char *prefix) {
    if (!prefix) return NULL;
    DIR *d = opendir(".");
    if (!d) return NULL;

    struct dirent *e;
    static char result[1024];
    int found = 0;
    size_t prelen = strlen(prefix);

    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, prelen) == 0) {
            if (!found) {
                strncpy(result, e->d_name, sizeof(result)-1);
                result[sizeof(result)-1] = '\0';
            } else {
                /* more than one match -> ambiguous */
                closedir(d);
                return NULL;
            }
            found++;
        }
    }
    closedir(d);
    return found == 1 ? result : NULL;
}

/* ---------- Pipeline execution ---------- */

static int execute_pipeline(pipeline_t *pl, char *rawline) {
    show_loading_bar("EXECUTING COMMAND");

    /* alias expansion: for each command in the pipeline, try to expand */
    for (int i = 0; i < pl->ncmds; i++) {
        if (pl->cmds[i].argc > 0) {
            char original_cmd[4096] = {0};
            for (int j = 0; j < pl->cmds[i].argc; j++) {
                if (j > 0) strncat(original_cmd, " ", sizeof(original_cmd) - strlen(original_cmd) - 1);
                strncat(original_cmd, pl->cmds[i].argv[j], sizeof(original_cmd) - strlen(original_cmd) - 1);
            }

            char *expanded = expand_aliases(original_cmd);
            if (strcmp(expanded, original_cmd) != 0) {
                int new_ntoks;
                char **new_toks = tokenize(expanded, &new_ntoks);

                for (int j = 0; j < pl->cmds[i].argc; j++) {
                    free(pl->cmds[i].argv[j]);
                }

                pl->cmds[i].argc = 0;
                for (int j = 0; j < new_ntoks && j < MAX_ARGS-1; j++) {
                    pl->cmds[i].argv[pl->cmds[i].argc++] = strdup_safe(new_toks[j]);
                }
                pl->cmds[i].argv[pl->cmds[i].argc] = NULL;

                free_tokens(new_toks, new_ntoks);
            }
            free(expanded);
        }
    }

    int n = pl->ncmds;
    int pipefds[2*(n>0?(n-1):0)];
    for (int i=0;i<n-1;i++) {
        if (pipe(pipefds + i*2) < 0) { perror("pipe"); return 1; }
    }

    pid_t pgid = 0;
    pid_t p;
    for (int i=0;i<n;i++) {
        cmd_t *c = &pl->cmds[i];
        if (c->argc == 0) continue;

        /* single built-in optimization (no redir/pipes/background) */
        if (n==1 && is_builtin(c->argv[0]) && !pl->background && !c->infile && !c->outfile) {
            return run_builtin(c->argc, c->argv);
        }

        p = fork();
        if (p < 0) { perror("fork"); return 1; }
        if (p == 0) {
            if (i==0) pgid = getpid();
            setpgid(0, pgid);

            if (!pl->background) tcsetpgrp(STDIN_FILENO, pgid);

            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (i>0) {
                dup2(pipefds[(i-1)*2], STDIN_FILENO);
            }
            if (i < n-1) {
                dup2(pipefds[i*2 + 1], STDOUT_FILENO);
            }

            for (int j=0;j<2*(n-1);j++) close(pipefds[j]);

            redirect_io(c->infile, c->outfile, c->append);

            if (is_builtin(c->argv[0])) {
                int rc = run_builtin(c->argc, c->argv);
                exit(rc);
            } else {
                execvp(c->argv[0], c->argv);
                fprintf(stderr, "mysh: command not found: %s\n", c->argv[0]);
                exit(127);
            }
        } else {
            if (i==0) {
                pgid = p;
            }
            setpgid(p, pgid);
        }
    }

    for (int j=0;j<2*(n-1);j++) if (j>=0) close(pipefds[j]);

    if (pl->background) {
        add_job(pgid, rawline, JOB_RUNNING);
        printf(CLR_DARK_GRAY "[" CLR_NEON_GREEN "BACKGROUND" CLR_DARK_GRAY "] " CLR_LIGHT_GRAY "Job [%d] started with PID %d\n" CLR_RESET,
               next_job_id-1, pgid);
    } else {
        fg_pgid = pgid;
        tcsetpgrp(STDIN_FILENO, pgid);

        int status;
        while (1) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                break;
            }
            if (WIFSTOPPED(status)) {
                add_job(pgid, rawline, JOB_STOPPED);
                break;
            }
            pid_t r = waitpid(-pgid, &status, WNOHANG);
            if (r == -1 || r == 0) {
                break;
            }
        }

        tcsetpgrp(STDIN_FILENO, shell_pgid);
        fg_pgid = 0;
    }

    return 0;
}

/* ---------- Simple input (fgets) ---------- */

/* You have a more advanced tab-readline earlier; to keep things stable
   we read the line simply. If you want arrow-key editing or live tab
   completion, next step (phase 2) is to re-enable the interactive reader. */
static char *read_line_with_tab_completion(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);

    static char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        return NULL;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') {
        buf[len-1] = '\0';
    }

    return strdup_safe(buf);
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) perror("setpgid");
    tcgetattr(STDIN_FILENO, &shell_tmodes);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    setup_signals();
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigtstp_handler);

    load_history();
    load_persistent_data();

    /* sample aliases you can enable if desired:
    add_alias("ll", "ls -l");
    add_alias("la", "ls -la");
    add_alias("..", "cd ..");
    add_alias("...", "cd ../..");
    add_alias("neo", "echo 'Wake up, Neo...'");
    */

    print_cyberpunk_header();

    char *line = NULL;
    int last_status = 0;
    int command_count = 0;

    while (1) {
        remove_done_jobs();
        char prompt[PROMPT_BUF];
        build_cyberpunk_prompt(prompt, sizeof(prompt), last_status);

        line = read_line_with_tab_completion(prompt);
        if (!line) {
            printf("\n");
            builtin_exit(0, NULL);
            break;
        }
        if (line[0] == '\0') { free(line); continue; }

        char *rawline = strdup_safe(line);
        if (rawline[0] == '!' && isdigit((unsigned char)rawline[1])) {
            int id = atoi(rawline+1);
            if (id>=1 && id<=history_count) {
                free(rawline);
                rawline = strdup_safe(history[id-1]);
                printf(CLR_DARK_GRAY "[" CLR_NEON_PURPLE "HISTORY" CLR_DARK_GRAY "] " CLR_LIGHT_GRAY "%s\n" CLR_RESET, rawline);
            } else {
                print_cyberpunk_error("no such history entry");
                free(rawline);
                free(line);
                continue;
            }
        }

        push_history(rawline);
        command_count++;

        check_achievements(rawline, command_count);

        size_t L = strlen(rawline);
        if (L>0 && rawline[L-1]=='?') {
            char *preview = strdup_safe(rawline);
            preview[L-1]=0; /* Remove the '?' for tokenization */

            char *expanded_preview = expand_aliases(preview);
            int ntoks;
            char **toks = tokenize(expanded_preview, &ntoks);

            printf(CLR_DARK_GRAY "┌─────────────────────────────────────────────────────────────────┐\n" CLR_RESET);
            printf(CLR_DARK_GRAY "│" CLR_NEON_CYAN "                        TOKEN PREVIEW                         " CLR_DARK_GRAY "│\n" CLR_RESET);
            printf(CLR_DARK_GRAY "├─────────────────────────────────────────────────────────────────┤\n" CLR_RESET);
            printf(CLR_DARK_GRAY "│ " CLR_LIGHT_GRAY);
            for (int i=0;i<ntoks;i++) {
                printf(" '%s'", toks[i]);
            }
            printf(CLR_DARK_GRAY " │\n");
            print_bottom_border();

            free_tokens(toks, ntoks);
            free(expanded_preview);
            free(preview);
            free(rawline);
            free(line);
            continue;
        }

        int ntoks;
        char **toks = tokenize(rawline, &ntoks);
        if (ntoks == 0) { free_tokens(toks,ntoks); free(rawline); free(line); continue; }
        pipeline_t pl = parse_tokens(toks, ntoks);

        last_status = execute_pipeline(&pl, rawline);

        free_tokens(toks, ntoks);
        free(rawline);
        free(line);
    }

    save_history();
    save_persistent_data();
    return 0;
}
