/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Architecture:
 *   - Single binary used as both supervisor daemon and CLI client
 *   - IPC: UNIX domain socket at /tmp/mini_runtime.sock
 *   - Isolation: clone(2) with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS
 *   - Logging: pipe per container → bounded buffer → dedicated logger thread
 *   - Signals: SIGCHLD for reaping, SIGINT/SIGTERM for graceful shutdown
 *   - Kernel integration: /dev/container_monitor via ioctl
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ─── constants ─────────────────────────────────────────────────────────── */
#define STACK_SIZE            (1024 * 1024)
#define CONTAINER_ID_LEN      32
#define CONTROL_PATH          "/tmp/mini_runtime.sock"
#define LOG_DIR               "logs"
#define CONTROL_MESSAGE_LEN   256
#define CHILD_COMMAND_LEN     256
#define LOG_CHUNK_SIZE        4096
#define LOG_BUFFER_CAPACITY   16
#define DEFAULT_SOFT_LIMIT    (40UL << 20)
#define DEFAULT_HARD_LIMIT    (64UL << 20)
#define MONITOR_DEV           "/dev/container_monitor"
#define PS_LINE_LEN           512
#define PS_MAX_CONTAINERS     64

/* ─── types ──────────────────────────────────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char                     id[CONTAINER_ID_LEN];
    pid_t                    host_pid;
    time_t                   started_at;
    container_state_t        state;
    unsigned long            soft_limit_bytes;
    unsigned long            hard_limit_bytes;
    int                      exit_code;
    int                      exit_signal;
    char                     log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

/* Serialised PS row sent back to the client */
typedef struct {
    char  id[CONTAINER_ID_LEN];
    pid_t host_pid;
    char  state[16];
    char  started_at[32];
    int   exit_code;
} ps_row_t;

typedef struct {
    int       count;
    ps_row_t  rows[PS_MAX_CONTAINERS];
} ps_response_t;

typedef struct {
    char  id[CONTAINER_ID_LEN];
    char  rootfs[PATH_MAX];
    char  command[CHILD_COMMAND_LEN];
    int   nice_value;
    int   log_write_fd;   /* write-end of the logging pipe */
} child_config_t;

/* Per-container pipe reader thread arg */
typedef struct {
    int                log_read_fd;
    char               container_id[CONTAINER_ID_LEN];
    bounded_buffer_t  *log_buffer;
} pipe_reader_arg_t;

typedef struct {
    int                  server_fd;
    int                  monitor_fd;
    volatile int         should_stop;
    pthread_t            logger_thread;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
} supervisor_ctx_t;

/* Global for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ─── usage ──────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

/* ─── argument parsing ───────────────────────────────────────────────────── */
static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                    "Invalid value for --nice (expected -20..19): %s\n",
                    argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                  return "unknown";
    }
}

/* ─── bounded buffer ─────────────────────────────────────────────────────── */
static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));

    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }

    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push - producer side.
 * Blocks while the buffer is full.
 * Returns 0 on success, -1 if shutdown was requested.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer side.
 * Blocks while the buffer is empty.
 * Returns 1 if an item was returned, 0 if shut down and buffer is drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0) {
        /* shutting_down and nothing left */
        pthread_mutex_unlock(&buf->mutex);
        return 0;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 1;
}

/* ─── logging consumer thread ────────────────────────────────────────────── */
/*
 * logging_thread - drains the bounded buffer and appends to per-container
 * log files under LOG_DIR/<container_id>.log
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t        item;
    char              path[PATH_MAX];
    int               fd;
    ssize_t           written;

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(buf, &item) == 1) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }
        written = 0;
        while ((size_t)written < item.length) {
            ssize_t n = write(fd, item.data + written,
                              item.length - (size_t)written);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("logging_thread: write");
                break;
            }
            written += n;
        }
        close(fd);
    }

    return NULL;
}

/* ─── pipe reader thread ─────────────────────────────────────────────────── */
/*
 * One of these runs per container.  It reads from the read-end of the
 * container's stdout/stderr pipe and pushes chunks into the shared
 * bounded buffer for the logger thread to persist.
 */
static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;
    log_item_t         item;
    ssize_t            n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

    while (1) {
        n = read(pra->log_read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;          /* EOF or error → container exited */
        item.length = (size_t)n;
        if (bounded_buffer_push(pra->log_buffer, &item) != 0)
            break;                   /* supervisor shutting down */
    }

    close(pra->log_read_fd);
    free(pra);
    return NULL;
}

/* ─── container child entrypoint ─────────────────────────────────────────── */
/*
 * Runs inside the cloned namespaces.
 * Sets up /proc, chroots, redirects stdout/stderr, execs the command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg  = (child_config_t *)arg;
    char           *argv[4];

    /* Apply nice value */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("child_fn: nice");
    }

    /* Redirect stdout and stderr to the write end of the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("child_fn: dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Mount /proc so tools inside the container work */
    if (mount("proc", "proc", "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        /* Non-fatal if already mounted; just warn */
        if (errno != EBUSY)
            perror("child_fn: mount proc (warning)");
    }

    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child_fn: chdir /");
        return 1;
    }

    /* Mount /proc inside chroot */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        if (errno != EBUSY)
            perror("child_fn: mount /proc inside chroot (warning)");
    }

    /* Set hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* Execute the requested command via /bin/sh -c so shell syntax works */
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = cfg->command;
    argv[3] = NULL;

    execv("/bin/sh", argv);
    perror("child_fn: execv");
    return 1;
}

/* ─── ioctl helpers ──────────────────────────────────────────────────────── */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid               = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ─── metadata helpers (caller must hold metadata_lock) ─────────────────── */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strncmp(r->id, id, CONTAINER_ID_LEN) == 0)
            return r;
        r = r->next;
    }
    return NULL;
}

static container_record_t *add_container(supervisor_ctx_t *ctx)
{
    container_record_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->next        = ctx->containers;
    ctx->containers = r;
    return r;
}

static void remove_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t **pp = &ctx->containers;
    while (*pp) {
        if (strncmp((*pp)->id, id, CONTAINER_ID_LEN) == 0) {
            container_record_t *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ─── SIGCHLD reaper ─────────────────────────────────────────────────────── */
static void sigchld_handler(int sig)
{
    (void)sig;
    /* The supervisor event loop calls waitpid; just wake it up via the flag */
    /* Actual reaping happens in the main loop to avoid async-signal-safety
     * issues with mutex operations. */
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ─── handle_start ───────────────────────────────────────────────────────── */
/*
 * Called on the supervisor side when a CMD_START or CMD_RUN request arrives.
 * Creates the container, spawns a clone, registers with the kernel monitor,
 * and starts the pipe reader thread.
 *
 * Returns 0 on success; fills resp.
 */
static int handle_start(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        control_response_t *resp)
{
    container_record_t *rec;
    child_config_t     *cfg;
    pipe_reader_arg_t  *pra;
    char               *stack;
    char               *stack_top;
    int                 pipefd[2];
    pid_t               pid;
    pthread_t           reader_tid;
    pthread_attr_t      attr;
    char                log_path[PATH_MAX];

    /* Ensure log dir exists */
    mkdir(LOG_DIR, 0755);

    pthread_mutex_lock(&ctx->metadata_lock);

    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' already exists", req->container_id);
        return -1;
    }

    rec = add_container(ctx);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->state            = CONTAINER_STARTING;
    rec->started_at       = time(NULL);
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(log_path, sizeof(log_path), "%s/%s.log",
             LOG_DIR, req->container_id);
    strncpy(rec->log_path, log_path, PATH_MAX - 1);

    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create stdout/stderr pipe for the container */
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        perror("handle_start: pipe2");
        pthread_mutex_lock(&ctx->metadata_lock);
        remove_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "pipe2 failed: %s",
                 strerror(errno));
        return -1;
    }

    /* Build child config – must stay alive until child has execvp'd */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        pthread_mutex_lock(&ctx->metadata_lock);
        remove_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value  = req->nice_value;
    cfg->log_write_fd = pipefd[1];   /* child writes here */

    /* Allocate clone stack */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        pthread_mutex_lock(&ctx->metadata_lock);
        remove_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }
    stack_top = stack + STACK_SIZE;  /* stack grows down */

    pid = clone(child_fn,
                stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    /* Close write-end in parent AFTER clone */
    close(pipefd[1]);
    free(stack);
    free(cfg);

    if (pid < 0) {
        perror("handle_start: clone");
        close(pipefd[0]);
        pthread_mutex_lock(&ctx->metadata_lock);
        remove_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "clone failed: %s", strerror(errno));
        return -1;
    }

    /* Update metadata */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container(ctx, req->container_id);
    if (rec) {
        rec->host_pid = pid;
        rec->state    = CONTAINER_RUNNING;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor (best-effort) */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd,
                                  req->container_id, pid,
                                  req->soft_limit_bytes,
                                  req->hard_limit_bytes) != 0)
            perror("handle_start: register_with_monitor (non-fatal)");
    }

    /* Start a pipe-reader thread to feed the bounded buffer */
    pra = calloc(1, sizeof(*pra));
    if (!pra) {
        /* Non-fatal: we just won't have logs */
        close(pipefd[0]);
    } else {
        pra->log_read_fd  = pipefd[0];
        pra->log_buffer   = &ctx->log_buffer;
        strncpy(pra->container_id, req->container_id, CONTAINER_ID_LEN - 1);

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&reader_tid, &attr, pipe_reader_thread, pra) != 0) {
            perror("handle_start: pthread_create pipe_reader");
            close(pipefd[0]);
            free(pra);
        }
        pthread_attr_destroy(&attr);
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "started container '%s' pid=%d", req->container_id, pid);
    return 0;
}

/* ─── handle_stop ────────────────────────────────────────────────────────── */
static int handle_stop(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       control_response_t *resp)
{
    container_record_t *rec;
    pid_t               pid = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container(ctx, req->container_id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' not found", req->container_id);
        return -1;
    }
    if (rec->state != CONTAINER_RUNNING &&
        rec->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' is not running (state=%s)",
                 req->container_id, state_to_string(rec->state));
        return -1;
    }
    pid = rec->host_pid;
    rec->state = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        perror("handle_stop: kill SIGTERM");
        /* Try harder */
        kill(pid, SIGKILL);
    }

    /* Unregister from kernel monitor */
    if (ctx->monitor_fd >= 0)
        unregister_from_monitor(ctx->monitor_fd, req->container_id, pid);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "sent SIGTERM to container '%s' (pid=%d)",
             req->container_id, pid);
    return 0;
}

/* ─── handle_ps ──────────────────────────────────────────────────────────── */
static int handle_ps(supervisor_ctx_t *ctx,
                     int client_fd)
{
    ps_response_t       psr;
    container_record_t *r;
    struct tm           tm_buf;
    int                 i = 0;

    memset(&psr, 0, sizeof(psr));

    pthread_mutex_lock(&ctx->metadata_lock);
    r = ctx->containers;
    while (r && i < PS_MAX_CONTAINERS) {
        strncpy(psr.rows[i].id,    r->id,    CONTAINER_ID_LEN - 1);
        psr.rows[i].host_pid   = r->host_pid;
        psr.rows[i].exit_code  = r->exit_code;
        strncpy(psr.rows[i].state, state_to_string(r->state), 15);
        localtime_r(&r->started_at, &tm_buf);
        strftime(psr.rows[i].started_at, sizeof(psr.rows[i].started_at),
                 "%Y-%m-%d %H:%M:%S", &tm_buf);
        i++;
        r = r->next;
    }
    psr.count = i;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Send ps_response_t directly on the socket */
    if (write(client_fd, &psr, sizeof(psr)) != sizeof(psr))
        perror("handle_ps: write");
    return 0;
}

/* ─── handle_logs ────────────────────────────────────────────────────────── */
static int handle_logs(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       int client_fd)
{
    container_record_t *rec;
    char                log_path[PATH_MAX];
    char                buf[4096];
    int                 fd;
    ssize_t             n;
    control_response_t  resp;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container(ctx, req->container_id);
    if (rec)
        strncpy(log_path, rec->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec) {
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' not found", req->container_id);
        write(client_fd, &resp, sizeof(resp));
        return -1;
    }

    fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "no log file for '%s'", req->container_id);
        write(client_fd, &resp, sizeof(resp));
        return -1;
    }

    /* First send a success sentinel */
    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "LOG_START");
    write(client_fd, &resp, sizeof(resp));

    /* Stream the log file to the client */
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(client_fd, buf + sent, (size_t)(n - sent));
            if (w < 0) { if (errno == EINTR) continue; goto done; }
            sent += w;
        }
    }
done:
    close(fd);
    return 0;
}

/* ─── reap children ──────────────────────────────────────────────────────── */
static void reap_children(supervisor_ctx_t *ctx)
{
    int    wstatus;
    pid_t  pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    r->state     = CONTAINER_EXITED;
                    r->exit_code = WEXITSTATUS(wstatus);
                } else if (WIFSIGNALED(wstatus)) {
                    r->state       = CONTAINER_KILLED;
                    r->exit_signal = WTERMSIG(wstatus);
                }
                /* Unregister from kernel monitor */
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, r->id, pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ─── supervisor event loop ──────────────────────────────────────────────── */
static int supervisor_loop(supervisor_ctx_t *ctx)
{
    struct sockaddr_un  peer_addr;
    socklen_t           peer_len;
    int                 client_fd;
    control_request_t   req;
    control_response_t  resp;
    fd_set              rfds;
    struct timeval      tv;
    int                 rc;

    while (!ctx->should_stop) {
        /* Reap any exited containers */
        reap_children(ctx);

        FD_ZERO(&rfds);
        FD_SET(ctx->server_fd, &rfds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        rc = select(ctx->server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("supervisor_loop: select");
            break;
        }
        if (rc == 0) continue;  /* timeout → loop again to check should_stop */

        peer_len  = sizeof(peer_addr);
        client_fd = accept(ctx->server_fd,
                           (struct sockaddr *)&peer_addr, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EWOULDBLOCK) continue;
            perror("supervisor_loop: accept");
            continue;
        }

        /* Read the control request */
        ssize_t nr = read(client_fd, &req, sizeof(req));
        if (nr != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        memset(&resp, 0, sizeof(resp));

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            handle_start(ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            break;

        case CMD_STOP:
            handle_stop(ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            break;

        case CMD_PS:
            handle_ps(ctx, client_fd);
            break;

        case CMD_LOGS:
            handle_logs(ctx, &req, client_fd);
            break;

        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "unknown command %d", req.kind);
            write(client_fd, &resp, sizeof(resp));
            break;
        }

        close(client_fd);
    }

    return 0;
}

/* ─── run_supervisor ─────────────────────────────────────────────────────── */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t    ctx;
    struct sockaddr_un  addr;
    struct sigaction    sa;
    int                 rc;

    (void)rootfs;   /* base rootfs noted but per-container rootfs is per-request */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Mutex for container metadata */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    /* Bounded log buffer */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor device (non-fatal if not loaded) */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open %s (%s); "
                        "memory monitoring disabled\n",
                MONITOR_DEV, strerror(errno));

    /* UNIX domain socket for control plane */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    unlink(CONTROL_PATH);   /* Remove stale socket */

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* Signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE (clients may disconnect) */
    signal(SIGPIPE, SIG_IGN);

    /* Logger consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL,
                        logging_thread, &ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "Supervisor started (base-rootfs=%s, socket=%s)\n",
            rootfs, CONTROL_PATH);

    /* Main event loop */
    supervisor_loop(&ctx);

    /* ── graceful shutdown ── */
    fprintf(stderr, "Supervisor shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING)
            kill(r->host_pid, SIGTERM);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait briefly for containers to exit */
    sleep(2);
    reap_children(&ctx);

    /* Flush the log buffer */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur);
        cur = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    rc = 0;
cleanup:
    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return rc;
}

/* ─── client-side control channel ───────────────────────────────────────── */
/*
 * Opens a connection to the supervisor socket, sends the request,
 * reads and prints the response, then exits.
 */
static int send_control_request(const control_request_t *req)
{
    int                client_fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t            n;

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(client_fd);
        return 1;
    }

    /* Send the request */
    if (write(client_fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write control request");
        close(client_fd);
        return 1;
    }

    /* Specialised read for CMD_PS */
    if (req->kind == CMD_PS) {
        ps_response_t psr;
        n = read(client_fd, &psr, sizeof(psr));
        if (n == (ssize_t)sizeof(psr)) {
            printf("%-16s %-8s %-12s %-20s %s\n",
                   "ID", "PID", "STATE", "STARTED", "EXIT");
            for (int i = 0; i < psr.count; i++) {
                printf("%-16s %-8d %-12s %-20s %d\n",
                       psr.rows[i].id,
                       psr.rows[i].host_pid,
                       psr.rows[i].state,
                       psr.rows[i].started_at,
                       psr.rows[i].exit_code);
            }
            if (psr.count == 0)
                printf("(no containers)\n");
        } else {
            fprintf(stderr, "Unexpected response from supervisor\n");
        }
        close(client_fd);
        return 0;
    }

    /* Specialised read for CMD_LOGS */
    if (req->kind == CMD_LOGS) {
        n = read(client_fd, &resp, sizeof(resp));
        if (n != (ssize_t)sizeof(resp)) {
            fprintf(stderr, "No response from supervisor\n");
            close(client_fd);
            return 1;
        }
        if (resp.status != 0) {
            fprintf(stderr, "Error: %s\n", resp.message);
            close(client_fd);
            return 1;
        }
        /* Stream the rest to stdout */
        char buf[4096];
        while ((n = read(client_fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)n, stdout);
        close(client_fd);
        return 0;
    }

    /* Default: one control_response_t back */
    n = read(client_fd, &resp, sizeof(resp));
    if (n == (ssize_t)sizeof(resp)) {
        if (resp.status == 0)
            printf("%s\n", resp.message);
        else
            fprintf(stderr, "Error: %s\n", resp.message);
    } else {
        fprintf(stderr, "Incomplete response from supervisor\n");
    }

    close(client_fd);
    return (resp.status == 0) ? 0 : 1;
}

/* ─── CMD handlers (client side) ─────────────────────────────────────────── */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s start <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind               = CMD_START;
    req.soft_limit_bytes   = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes   = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s run <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind               = CMD_RUN;
    req.soft_limit_bytes   = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes   = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}


