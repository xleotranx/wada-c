#include "wada_instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

struct wada_instance {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
    long pid;
};

static void lock_path(const char *data_dir, char *out, size_t cap) {
    snprintf(out, cap, "%s" WADA_PATH_SEP "wada.lock", data_dir ? data_dir : ".");
}

static long current_pid(void) {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

/* The pid is diagnostics, not authority: it lets the second instance say who
 * holds the directory. Failure to write it does not fail the lock. */
static void write_pid(void *handle, long pid) {
#ifdef _WIN32
    HANDLE h = (HANDLE)handle;
    char text[32];
    int n = snprintf(text, sizeof text, "%ld\n", pid);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    DWORD written = 0;
    WriteFile(h, text, (DWORD)n, &written, NULL);
#else
    int fd = (int)(intptr_t)handle;
    char text[32];
    int n = snprintf(text, sizeof text, "%ld\n", pid);
    if (lseek(fd, 0, SEEK_SET) == 0) {
        ssize_t ignored = write(fd, text, (size_t)n);
        (void)ignored;
        if (ftruncate(fd, n) != 0) { /* content is best effort */ }
    }
#endif
}

wada_instance *wada_instance_acquire(const char *data_dir, wada_err *err) {
    char path[800];
    lock_path(data_dir, path, sizeof path);

    wada_instance *lock = (wada_instance *)calloc(1, sizeof *lock);
    if (!lock) {
        wada_err_set(err, WADA_ERR_NOMEM, "instance lock");
        return NULL;
    }
    lock->pid = current_pid();

#ifdef _WIN32
    lock->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0 /* no sharing */, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock->handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        lock->handle = NULL;
        free(lock);
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
            wada_err_set(err, WADA_ERR_STATE,
                         "another Wada instance is already using this data directory");
            return NULL;
        }
        wada_err_set(err, WADA_ERR_IO, "cannot create lock file (error %lu)", (unsigned long)code);
        return NULL;
    }
    write_pid(lock->handle, lock->pid);
#else
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        free(lock);
        wada_err_set(err, WADA_ERR_IO, "cannot create lock file: %s", strerror(errno));
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int code = errno;
        close(fd);
        free(lock);
        if (code == EWOULDBLOCK) {
            wada_err_set(err, WADA_ERR_STATE,
                         "another Wada instance is already using this data directory");
            return NULL;
        }
        wada_err_set(err, WADA_ERR_IO, "cannot lock: %s", strerror(code));
        return NULL;
    }
    lock->fd = fd;
    write_pid((void *)(intptr_t)fd, lock->pid);
#endif
    return lock;
}

void wada_instance_release(wada_instance *lock) {
    if (!lock) return;
#ifdef _WIN32
    if (lock->handle) CloseHandle(lock->handle);
#else
    if (lock->fd > 0) close(lock->fd);
#endif
    free(lock);
}

long wada_instance_pid(const wada_instance *lock) {
    return lock ? lock->pid : 0;
}
