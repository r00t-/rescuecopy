/*
 * ddrescue-like media recovery tool in C (Unix)
 *
 * Compile: cc -O2 -o rescue rescue.c -lm
 * Usage:   ./rescue [options] input_file output_file
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <termios.h>
#include <ctype.h>
#include <limits.h>

/* ?? Constants ??????????????????????????????????????????????????????? */

#define PROGRAM_NAME    "rescue"
#define PROGRAM_VERSION "1.0.0"
#define META_MAGIC      "RESCUE-META"
#define META_VERSION    1

#define DEFAULT_SECTOR_SIZE       512
#define DEFAULT_BUFFER_SIZE       (32ULL * 1024 * 1024)
#define DEFAULT_MAX_FWD_SEGMENT   (4ULL * 1024 * 1024)
#define DEFAULT_MAX_RETRIES       254   /* max storable: 254 (byte val 255 = 254 fails) */
#define DEFAULT_SKIP_RATIO        (1.0 / 3.0)

#define STATUS_NOT_COPIED  0x00
#define STATUS_COPIED      0x01
/* 0x02 .. 0xFF  =>  (value - 1) failed attempts */
#define STATUS_MAX_FAILS   254  /* stored as 0xFF */

#define FAIL_COUNT(v)  ((v) >= 2 ? (int)((v) - 1) : 0)
#define FAIL_BYTE(n)   ((uint8_t)((n) + 1))  /* n >= 1 */

/* ?? Global state ???????????????????????????????????????????????????? */

static volatile sig_atomic_t g_interrupted = 0;

/* Parameters */
static const char *g_input_path    = NULL;
static const char *g_output_path   = NULL;
static char        g_meta_path[PATH_MAX];
static char        g_meta_bak_path[PATH_MAX];

static uint64_t g_sector_size       = DEFAULT_SECTOR_SIZE;
static uint64_t g_buffer_size       = DEFAULT_BUFFER_SIZE;
static uint64_t g_max_fwd_segment   = DEFAULT_MAX_FWD_SEGMENT;
static int      g_max_retries       = DEFAULT_MAX_RETRIES;
static int      g_max_total_errors  = -1;  /* unlimited */
static int      g_max_cont_errors   = -1;  /* unlimited */
static uint64_t g_min_area_size     = 0;   /* 0 = no override */
static double   g_skip_ratio        = DEFAULT_SKIP_RATIO;
static bool     g_zero_init         = false;
static bool     g_reset_retries     = false;
static bool     g_show_status       = false;

/* Runtime */
static int      g_fd_in  = -1;
static int      g_fd_out = -1;
static uint64_t g_file_size    = 0;
static uint64_t g_sector_count = 0;
static uint8_t *g_status_map   = NULL;  /* one byte per sector */
static uint8_t *g_io_buffer    = NULL;

static int      g_total_errors = 0;
static int      g_cont_errors  = 0;
static bool     g_first_pass   = true;  /* initial startup flag */

static uint64_t g_sectors_copied   = 0;
static uint64_t g_sectors_failed   = 0;
static uint64_t g_sectors_untried  = 0;
static uint64_t g_bytes_read_ok    = 0;

/* Status display */
static int      g_term_rows = 24;
static int      g_term_cols = 80;
static struct timeval g_start_time;

/* ?? Signal handler ?????????????????????????????????????????????????? */

static void signal_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

/* ?? Utility: parse size with suffix ????????????????????????????????? */

static int parse_size(const char *str, uint64_t *out)
{
    char *end = NULL;
    double val = strtod(str, &end);
    if (end == str || val < 0)
        return -1;

    uint64_t mult = 1;
    if (*end) {
        switch (*end) {
        case 'k': mult = 1000ULL; break;
        case 'm': mult = 1000000ULL; break;
        case 'g': mult = 1000000000ULL; break;
        case 't': mult = 1000000000000ULL; break;
        case 'K': mult = 1024ULL; break;
        case 'M': mult = 1024ULL * 1024; break;
        case 'G': mult = 1024ULL * 1024 * 1024; break;
        case 'T': mult = 1024ULL * 1024 * 1024 * 1024; break;
        default:
            return -1;
        }
        end++;
    }
    if (*end != '\0')
        return -1;

    *out = (uint64_t)(val * (double)mult);
    return 0;
}

static int parse_int_size(const char *str, int *out)
{
    uint64_t v;
    if (parse_size(str, &v) < 0)
        return -1;
    if (v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

/* ?? Utility: format size for display ???????????????????????????????? */

static const char *fmt_size(uint64_t bytes, char *buf, size_t bufsz)
{
    if (bytes >= 1000000000ULL)
        snprintf(buf, bufsz, "%.2f GB", (double)bytes / 1e9);
    else if (bytes >= 1000000ULL)
        snprintf(buf, bufsz, "%.2f MB", (double)bytes / 1e6);
    else if (bytes >= 1000ULL)
        snprintf(buf, bufsz, "%.2f KB", (double)bytes / 1e3);
    else
        snprintf(buf, bufsz, "%llu B", (unsigned long long)bytes);
    return buf;
}

/* ?? Terminal helpers ???????????????????????????????????????????????? */

static void get_terminal_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_term_rows = ws.ws_row;
        g_term_cols = ws.ws_col;
    }
}

/* ?? Metadata file handling ?????????????????????????????????????????? */

/*
 * Format:
 *   Line 1: "RESCUE-META v1"
 *   Line 2: "input_size: <decimal>"
 *   Line 3: "sector_size: <decimal>"
 *   Line 4: "sector_count: <decimal>"
 *   Line 5: "input_file: <path>"
 *   Line 6: "output_file: <path>"
 *   Line 7: empty line
 *   Binary: sector_count bytes of status data
 */

static int meta_write(const char *path)
{
    /* Write to a temp file, then rename for atomicity */
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create metadata temp file '%s': %s\n",
                tmp_path, strerror(errno));
        return -1;
    }

    char header[4096];
    int hlen = snprintf(header, sizeof(header),
        "%s v%d\n"
        "input_size: %llu\n"
        "sector_size: %llu\n"
        "sector_count: %llu\n"
        "input_file: %s\n"
        "output_file: %s\n"
        "\n",
        META_MAGIC, META_VERSION,
        (unsigned long long)g_file_size,
        (unsigned long long)g_sector_size,
        (unsigned long long)g_sector_count,
        g_input_path,
        g_output_path);

    ssize_t wr = write(fd, header, hlen);
    if (wr != hlen) goto write_err;

    uint64_t remaining = g_sector_count;
    uint64_t offset = 0;
    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > 1048576) chunk = 1048576;
        wr = write(fd, g_status_map + offset, chunk);
        if (wr < 0) goto write_err;
        offset += wr;
        remaining -= wr;
    }

    //if (fsync(fd) < 0) goto write_err;
    close(fd);

    if (rename(tmp_path, path) < 0) {
        fprintf(stderr, "Error: cannot rename '%s' to '%s': %s\n",
                tmp_path, path, strerror(errno));
        return -1;
    }
    return 0;

write_err:
    fprintf(stderr, "Error: writing metadata file '%s': %s\n", tmp_path, strerror(errno));
    close(fd);
    unlink(tmp_path);
    return -1;
}

static int meta_save(void)
{
    return meta_write(g_meta_path);
}

static int meta_save_backup(void)
{
    return meta_write(g_meta_bak_path);
}

static int meta_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    /* Read header line by line */
    char hbuf[8192];
    ssize_t rd = read(fd, hbuf, sizeof(hbuf) - 1);
    if (rd <= 0) { close(fd); return -1; }
    hbuf[rd] = '\0';

    /* Find end of header (empty line = "\n\n") */
    char *body = strstr(hbuf, "\n\n");
    if (!body) { close(fd); return -1; }
    body += 2; /* skip the two newlines */

    /* Parse header fields */
    int version = 0;
    unsigned long long input_size = 0, sector_size = 0, sector_count = 0;
    char in_file[PATH_MAX] = {0}, out_file[PATH_MAX] = {0};

    char *line = hbuf;
    int line_no = 0;
    char *next;
    while (line < body) {
        next = strchr(line, '\n');
        if (!next) break;
        *next = '\0';

        if (line_no == 0) {
            if (sscanf(line, META_MAGIC " v%d", &version) != 1 || version != META_VERSION) {
                fprintf(stderr, "Error: metadata version mismatch in '%s'\n", path);
                close(fd);
                return -1;
            }
        } else if (strncmp(line, "input_size: ", 12) == 0) {
            input_size = strtoull(line + 12, NULL, 10);
        } else if (strncmp(line, "sector_size: ", 13) == 0) {
            sector_size = strtoull(line + 13, NULL, 10);
        } else if (strncmp(line, "sector_count: ", 14) == 0) {
            sector_count = strtoull(line + 14, NULL, 10);
        } else if (strncmp(line, "input_file: ", 12) == 0) {
            strncpy(in_file, line + 12, sizeof(in_file) - 1);
        } else if (strncmp(line, "output_file: ", 13) == 0) {
            strncpy(out_file, line + 13, sizeof(out_file) - 1);
        }

        line = next + 1;
        line_no++;
    }

    /* Validate */
    if (input_size != g_file_size) {
        fprintf(stderr, "Error: metadata input_size mismatch (expected %llu, got %llu)\n",
                (unsigned long long)g_file_size, input_size);
        close(fd);
        return -1;
    }
    if (sector_size != g_sector_size) {
        fprintf(stderr, "Error: metadata sector_size mismatch (expected %llu, got %llu)\n",
                (unsigned long long)g_sector_size, sector_size);
        close(fd);
        return -1;
    }
    if (sector_count != g_sector_count) {
        fprintf(stderr, "Error: metadata sector_count mismatch (expected %llu, got %llu)\n",
                (unsigned long long)g_sector_count, sector_count);
        close(fd);
        return -1;
    }
    if (strcmp(in_file, g_input_path) != 0) {
        fprintf(stderr, "Warning: metadata input_file differs ('%s' vs '%s')\n",
                in_file, g_input_path);
    }
    if (strcmp(out_file, g_output_path) != 0) {
        fprintf(stderr, "Warning: metadata output_file differs ('%s' vs '%s')\n",
                out_file, g_output_path);
    }

    /* Read binary status blob */
    /* Some of it may already be in hbuf after the header */
    size_t header_len = (size_t)(body - hbuf);
    size_t data_in_buf = (size_t)rd - header_len;
    if (data_in_buf > g_sector_count)
        data_in_buf = g_sector_count;

    memcpy(g_status_map, body, data_in_buf);

    uint64_t remaining = g_sector_count - data_in_buf;
    uint64_t offset = data_in_buf;

    /* Seek to correct position in file */
    if (remaining > 0) {
        off_t file_offset = (off_t)(header_len + data_in_buf);
        if (lseek(fd, file_offset, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        while (remaining > 0) {
            size_t chunk = remaining;
            if (chunk > 1048576) chunk = 1048576;
            rd = read(fd, g_status_map + offset, chunk);
            if (rd <= 0) {
                fprintf(stderr, "Error: short read on metadata file '%s'\n", path);
                close(fd);
                return -1;
            }
            offset += rd;
            remaining -= rd;
        }
    }

    close(fd);
    return 0;
}

/* ?? Recount sector statistics from status map ??????????????????????? */

static void recount_stats(void)
{
    g_sectors_copied = 0;
    g_sectors_failed = 0;
    g_sectors_untried = 0;
    for (uint64_t i = 0; i < g_sector_count; i++) {
        uint8_t v = g_status_map[i];
        if (v == STATUS_COPIED)
            g_sectors_copied++;
        else if (v == STATUS_NOT_COPIED)
            g_sectors_untried++;
        else
            g_sectors_failed++;
    }
}

/* ?? Find largest uncopied range ????????????????????????????????????? */

/*
 * An "uncopied" sector has status STATUS_NOT_COPIED or status >= 2
 * with fail count < max_retries (i.e. still eligible for retry).
 * For the "largest range" scan during the fast copy phase,
 * only STATUS_NOT_COPIED sectors are considered.
 * For retry phases, sectors with fails < max_retries are also included.
 */

static bool sector_needs_copy(uint64_t idx, bool include_failed)
{
    uint8_t v = g_status_map[idx];
    if (v == STATUS_NOT_COPIED) return true;
    if (v == STATUS_COPIED) return false;
    if (!include_failed) return false;
    return FAIL_COUNT(v) < g_max_retries;
}

/* Find the largest contiguous range of sectors needing copy.
 * Returns length in sectors, sets *start_out. Returns 0 if none found. */
static uint64_t find_largest_range(uint64_t *start_out, bool include_failed)
{
    uint64_t best_start = 0, best_len = 0;
    uint64_t cur_start = 0, cur_len = 0;
    bool in_range = false;

    for (uint64_t i = 0; i < g_sector_count; i++) {
        if (sector_needs_copy(i, include_failed)) {
            if (!in_range) {
                cur_start = i;
                cur_len = 0;
                in_range = true;
            }
            cur_len++;
        } else {
            if (in_range && cur_len > best_len) {
                best_start = cur_start;
                best_len = cur_len;
            }
            in_range = false;
        }
    }
    if (in_range && cur_len > best_len) {
        best_start = cur_start;
        best_len = cur_len;
    }

    *start_out = best_start;
    return best_len;
}

/* Check if the sector before 'start' is a known-bad sector (failed reads) */
static bool preceded_by_bad(uint64_t start)
{
    if (start == 0) return false;
    uint8_t v = g_status_map[start - 1];
    return (v >= 2);
}

/* ?? I/O operations ?????????????????????????????????????????????????? */

/* Read a single sector. Returns 0 on success, -1 on error. */
static int read_sector(uint64_t sector_idx, void *buf)
{
    off_t off = (off_t)(sector_idx * g_sector_size);
    uint64_t to_read = g_sector_size;
    /* Handle last sector which may be shorter */
    if (sector_idx == g_sector_count - 1) {
        uint64_t remainder = g_file_size - (uint64_t)off;
        if (remainder < to_read)
            to_read = remainder;
    }

    if (lseek(g_fd_in, off, SEEK_SET) < 0)
        return -1;

    ssize_t rd = read(g_fd_in, buf, to_read);
    if (rd < 0 || (uint64_t)rd != to_read)
        return -1;

    return 0;
}

/* Read multiple sectors into buffer. Returns number of sectors successfully read
 * before an error, or the full count on success. On error, *error_sector is set
 * to the sector index (relative to start) that failed. */
static uint64_t read_sectors(uint64_t start_sector, uint64_t count, void *buf,
                             uint64_t *error_sector)
{
    off_t off = (off_t)(start_sector * g_sector_size);
    uint64_t total_bytes = count * g_sector_size;
    /* Adjust for last sector */
    if (start_sector + count >= g_sector_count) {
        uint64_t end_off = g_file_size;
        total_bytes = end_off - (uint64_t)off;
    }

    if (lseek(g_fd_in, off, SEEK_SET) < 0) {
        if (error_sector) *error_sector = 0;
        return 0;
    }

    /* Try to read the whole buffer at once */
    ssize_t rd = read(g_fd_in, buf, total_bytes);
    if (rd < 0) {
        /* Complete failure on first sector */
        if (error_sector) *error_sector = 0;
        return 0;
    }
    if ((uint64_t)rd == total_bytes) {
        return count; /* all good */
    }

    /* Partial read: figure out how many full sectors were read */
    uint64_t sectors_read = (uint64_t)rd / g_sector_size;
    if (error_sector) *error_sector = sectors_read;
    return sectors_read;
}

/* Write sectors to output. Returns 0 on success, -1 on error. */
static int write_sectors(uint64_t start_sector, uint64_t count, const void *buf)
{
    off_t off = (off_t)(start_sector * g_sector_size);
    uint64_t total_bytes = count * g_sector_size;
    if (start_sector + count >= g_sector_count) {
        uint64_t end_off = g_file_size;
        total_bytes = end_off - (uint64_t)off;
    }

    if (lseek(g_fd_out, off, SEEK_SET) < 0)
        return -1;

    const uint8_t *p = (const uint8_t *)buf;
    uint64_t remaining = total_bytes;
    while (remaining > 0) {
        ssize_t wr = write(g_fd_out, p, remaining);
        if (wr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += wr;
        remaining -= wr;
    }
    return 0;
}

/* Write a single sector to output */
static int write_sector(uint64_t sector_idx, const void *buf)
{
    return write_sectors(sector_idx, 1, buf);
}

/* ?? Mark sectors ???????????????????????????????????????????????????? */

static void mark_copied(uint64_t sector_idx)
{
    if (g_status_map[sector_idx] != STATUS_COPIED) {
        if (g_status_map[sector_idx] >= 2)
            g_sectors_failed--;
        else
            g_sectors_untried--;
        g_status_map[sector_idx] = STATUS_COPIED;
        g_sectors_copied++;
    }
}

static void mark_failed(uint64_t sector_idx)
{
    uint8_t v = g_status_map[sector_idx];
    if (v == STATUS_COPIED) return; /* should not happen */

    int fails;
    if (v == STATUS_NOT_COPIED) {
        fails = 1;
        g_sectors_untried--;
        g_sectors_failed++;
    } else {
        fails = FAIL_COUNT(v) + 1;
    }
    if (fails > STATUS_MAX_FAILS)
        fails = STATUS_MAX_FAILS;
    g_status_map[sector_idx] = FAIL_BYTE(fails);
}

/* ?? Status display ?????????????????????????????????????????????????? */

static void display_status(char mark, uint64_t current_sector)
{
    if (!g_show_status) return;

    get_terminal_size();

    int map_chars = (g_term_rows - 1) * g_term_cols - 1;
    if (map_chars < 1) return;

    struct timeval now;
    gettimeofday(&now, NULL);
    double elapsed = (now.tv_sec - g_start_time.tv_sec)
                   + (now.tv_usec - g_start_time.tv_usec) / 1e6;

    char sz1[32], sz2[32], sz3[32];
    fmt_size(g_sectors_copied * g_sector_size, sz1, sizeof(sz1));
    fmt_size(g_file_size, sz2, sizeof(sz2));
    if (elapsed > 0.01)
        fmt_size((uint64_t)(g_bytes_read_ok / elapsed), sz3, sizeof(sz3));
    else
        strcpy(sz3, "---");

    /* Move cursor to top-left */
    printf("\033[H");

    /* Status line (first row) */
    char status_line[1024];
    snprintf(status_line, sizeof(status_line),
        "Copied: %s/%s (%llu/%llu sectors)  Err: %d  Untried: %llu  Speed: %s/s  Elapsed: %.0fs",
        sz1, sz2,
        (unsigned long long)g_sectors_copied,
        (unsigned long long)g_sector_count,
        g_total_errors,
        (unsigned long long)g_sectors_untried,
        sz3, elapsed);

    /* Pad to fill line */
    int slen = (int)strlen(status_line);
    if (slen < g_term_cols) {
        memset(status_line + slen, ' ', g_term_cols - slen);
        status_line[g_term_cols] = '\0';
    }
    printf("\033[7m%.*s\033[0m", g_term_cols, status_line);

    /* Sector map: each character represents a group of sectors */
    double sectors_per_char = (double)g_sector_count / (double)map_chars;
    int current = (int)((double)current_sector / sectors_per_char);

    for (int i = 0; i < map_chars; i++) {
        if (mark && i == current){
            putchar(mark);
            continue;
        }
        uint64_t s_start = (uint64_t)((double)i * sectors_per_char);
        uint64_t s_end   = (uint64_t)((double)(i + 1) * sectors_per_char);
        if (s_end > g_sector_count) s_end = g_sector_count;
        if (s_start >= g_sector_count) s_start = g_sector_count - 1;
        if (s_end <= s_start) s_end = s_start + 1;

        uint64_t n_copied = 0, n_failed = 0, n_untried = 0;
        for (uint64_t s = s_start; s < s_end; s++) {
            uint8_t v = g_status_map[s];
            if (v == STATUS_COPIED) n_copied++;
            else if (v == STATUS_NOT_COPIED) n_untried++;
            else n_failed++;
        }

        uint64_t total = s_end - s_start;
        char ch;
        if (n_copied == total) {
            ch = '#';  /* fully copied */
        } else if (n_failed > 0 && n_copied == 0 && n_untried == 0) {
            ch = 'X';  /* all failed */
        } else if (n_failed > 0) {
            ch = '!';  /* some failed */
        } else if (n_copied > 0) {
            ch = '.';  /* partially copied */
        } else {
            ch = '-';  /* untried */
        }
        putchar(ch);
    }

    fflush(stdout);
}

/* ?? Error limit checking ???????????????????????????????????????????? */

typedef enum {
    ERR_OK = 0,
    ERR_TOTAL_LIMIT,
    ERR_CONT_LIMIT,
    ERR_INTERRUPTED
} err_check_t;

static err_check_t check_limits(void)
{
    if (g_interrupted)
        return ERR_INTERRUPTED;
    if (g_max_total_errors >= 0 && g_total_errors >= g_max_total_errors)
        return ERR_TOTAL_LIMIT;
    if (g_max_cont_errors >= 0 && g_cont_errors >= g_max_cont_errors)
        return ERR_CONT_LIMIT;
    return ERR_OK;
}

static void record_read_error(uint64_t sector_idx)
{
    mark_failed(sector_idx);
    g_total_errors++;
    g_cont_errors++;
}

static void record_read_success(uint64_t count)
{
    g_cont_errors = 0;
    g_bytes_read_ok += count * g_sector_size;
}

/* ?? Phase 1: Fast large-range copy ?????????????????????????????????? */

static err_check_t fast_copy_range(uint64_t start, uint64_t count)
{
    uint64_t sectors_per_buf = g_buffer_size / g_sector_size;
    uint64_t pos = start;
    uint64_t end = start + count;

    while (pos < end) {
        err_check_t ec = check_limits();
        if (ec != ERR_OK) return ec;

        uint64_t chunk = end - pos;
        if (chunk > sectors_per_buf) chunk = sectors_per_buf;

        /* Skip already-copied sectors at the beginning of this chunk */
        while (chunk > 0 && g_status_map[pos] == STATUS_COPIED) {
            pos++;
            chunk--;
        }
        if (chunk == 0) break;

        /* Find how many contiguous un-copied sectors from pos */
        uint64_t run = 0;
        while (run < chunk && g_status_map[pos + run] != STATUS_COPIED) {
            run++;
        }
        chunk = run;
        if (chunk > sectors_per_buf) chunk = sectors_per_buf;

        display_status('>', pos);

        uint64_t err_sect = 0;
        uint64_t ok = read_sectors(pos, chunk, g_io_buffer, &err_sect);

        if (ok > 0) {
            /* Write the successfully read data */
            if (write_sectors(pos, ok, g_io_buffer) < 0) {
                fprintf(stderr, "Error: write failed at sector %llu: %s\n",
                        (unsigned long long)pos, strerror(errno));
                return ERR_INTERRUPTED;
            }
            for (uint64_t i = 0; i < ok; i++)
                mark_copied(pos + i);
            record_read_success(ok);
        }

        if (ok < chunk) {
            /* Error at sector pos + ok (or err_sect) */
            uint64_t bad = pos + (ok > 0 ? ok : err_sect);
            if (bad < end && g_status_map[bad] != STATUS_COPIED) {
                record_read_error(bad);
            }

            /* Save progress and return to let caller pick next range */
            meta_save();
            meta_save_backup();
            display_status(0, 0);
            return ERR_OK;
        }

        pos += chunk;
        meta_save();
        meta_save_backup();
        display_status(0, 0);
    }

    return ERR_OK;
}

static err_check_t phase1(void)
{
    for (;;) {
        err_check_t ec = check_limits();
        if (ec != ERR_OK) return ec;

        uint64_t start, len;
        len = find_largest_range(&start, false);

        uint64_t min_sectors = g_max_fwd_segment / g_sector_size;
        if (g_min_area_size > 0) {
            uint64_t min2 = g_min_area_size / g_sector_size;
            if (min2 > min_sectors) min_sectors = min2;
        }
        if (min_sectors == 0) min_sectors = 1;

        if (len < min_sectors) break;

        /* If range starts right after a bad sector and this is not first pass,
         * apply skip ratio */
        if (!g_first_pass && preceded_by_bad(start)) {
            uint64_t skip = (uint64_t)((double)len * g_skip_ratio);
            if (skip > 0 && skip < len) {
                start += skip;
                len -= skip;
            }
        }
        g_first_pass = false;

        if (len < min_sectors) break;

        display_status('>', start);
        ec = fast_copy_range(start, len);
        if (ec != ERR_OK) return ec;
    }
    return ERR_OK;
}

/* ?? Phase 2: Split-and-copy (bidirectional from middle) ????????????? */

static err_check_t copy_forward_single(uint64_t start, uint64_t count,
                                        uint64_t *sectors_read)
{
    *sectors_read = 0;
    uint64_t pos = start;
    uint64_t end = start + count;

    display_status('>', start);
    while (pos < end) {
        err_check_t ec = check_limits();
        if (ec != ERR_OK) return ec;

        if (g_status_map[pos] == STATUS_COPIED) {
            pos++;
            continue;
        }

        if (read_sector(pos, g_io_buffer) < 0) {
            record_read_error(pos);
            meta_save();
            display_status(0, 0);
            return ERR_OK; /* stop forward, let caller decide */
        }

        if (write_sector(pos, g_io_buffer) < 0) {
            fprintf(stderr, "Error: write failed at sector %llu: %s\n",
                    (unsigned long long)pos, strerror(errno));
            return ERR_INTERRUPTED;
        }

        mark_copied(pos);
        record_read_success(1);
        (*sectors_read)++;
        pos++;
    }

    meta_save();
    meta_save_backup();
    display_status(0, 0);
    return ERR_OK;
}

static err_check_t copy_backward_single(uint64_t start, uint64_t count,
                                          uint64_t *sectors_read)
{
    *sectors_read = 0;
    if (count == 0) return ERR_OK;

    uint64_t pos = start + count; /* one past end, will decrement first */
    
    display_status('<', pos-1);

    while (pos > start) {
        pos--;

        err_check_t ec = check_limits();
        if (ec != ERR_OK) return ec;

        if (g_status_map[pos] == STATUS_COPIED)
            continue;

        if (read_sector(pos, g_io_buffer) < 0) {
            record_read_error(pos);
            meta_save();
            display_status(0, 0);
            return ERR_OK;
        }

        if (write_sector(pos, g_io_buffer) < 0) {
            fprintf(stderr, "Error: write failed at sector %llu: %s\n",
                    (unsigned long long)pos, strerror(errno));
            return ERR_INTERRUPTED;
        }

        mark_copied(pos);
        record_read_success(1);
        (*sectors_read)++;
    }

    meta_save();
    meta_save_backup();
    display_status(0, 0);
    return ERR_OK;
}

static err_check_t phase2(void)
{
    for (;;) {
        err_check_t ec = check_limits();
        if (ec != ERR_OK) return ec;

        uint64_t start, len;
        len = find_largest_range(&start, false);
        if (len == 0) break;

        uint64_t mid = start + len / 2;

        /* Forward from middle */
        uint64_t fwd_read = 0;
        ec = copy_forward_single(mid, start + len - mid, &fwd_read);
        if (ec != ERR_OK) return ec;

        /* Backward from middle if forward read anything */
        if (fwd_read > 0 && mid > start) {
            uint64_t bwd_read = 0;
            ec = copy_backward_single(start, mid - start, &bwd_read);
            if (ec != ERR_OK) return ec;
        }

        /* If nothing was read in forward, this range is probably all bad;
         * mark it as tried by attempting each sector once */
        // FIXME: above comment makes no sense, should have been marked bad by copy attempt
        if (fwd_read == 0) {
            /* Try the middle sector to mark it */
            if (g_status_map[mid] != STATUS_COPIED && sector_needs_copy(mid, false)) {
                display_status('*',mid);
                if (read_sector(mid, g_io_buffer) < 0) {
                    record_read_error(mid);
                } else {
                    if (write_sector(mid, g_io_buffer) == 0) {
                        mark_copied(mid);
                        record_read_success(1);
                    }
                }
                meta_save();
                display_status(0, 0);
            }
            /* Avoid infinite loop: if nothing can be copied, break */
            uint64_t new_len = find_largest_range(&start, false);
            if (new_len >= len) break; /* no progress */
        }
    }
    return ERR_OK;
}

/* ?? Phase 3: Retry failed sectors ??????????????????????????????????? */

static err_check_t phase3(void)
{
    /* Collect indices of retryable sectors */
    uint64_t *retryable = NULL;
    uint64_t n_retryable = 0;

    for (uint64_t i = 0; i < g_sector_count; i++) {
        uint8_t v = g_status_map[i];
        if (v == STATUS_COPIED || v == STATUS_NOT_COPIED) continue;
        if (FAIL_COUNT(v) < g_max_retries) {
            n_retryable++;
        }
    }

    if (n_retryable == 0) return ERR_OK;

    retryable = malloc(n_retryable * sizeof(uint64_t));
    if (!retryable) {
        fprintf(stderr, "Error: out of memory for retry list\n");
        return ERR_INTERRUPTED;
    }

    uint64_t idx = 0;
    for (uint64_t i = 0; i < g_sector_count; i++) {
        uint8_t v = g_status_map[i];
        if (v == STATUS_COPIED || v == STATUS_NOT_COPIED) continue;
        if (FAIL_COUNT(v) < g_max_retries) {
            retryable[idx++] = i;
        }
    }

    /* Fisher-Yates shuffle */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (uint64_t i = n_retryable - 1; i > 0; i--) {
        uint64_t j = (uint64_t)rand() % (i + 1);
        uint64_t tmp = retryable[i];
        retryable[i] = retryable[j];
        retryable[j] = tmp;
    }

    bool progress = true;
    while (progress) {
        progress = false;
        for (uint64_t i = 0; i < n_retryable; i++) {
            err_check_t ec = check_limits();
            if (ec != ERR_OK) { free(retryable); return ec; }

            uint64_t s = retryable[i];
            uint8_t v = g_status_map[s];
            if (v == STATUS_COPIED) continue;
            if (v >= 2 && FAIL_COUNT(v) >= g_max_retries) continue;

            display_status('*',s);
            if (read_sector(s, g_io_buffer) < 0) {
                record_read_error(s);
                display_status(0, 0);
                continue;
            }

            if (write_sector(s, g_io_buffer) < 0) {
                fprintf(stderr, "Error: write failed at sector %llu: %s\n",
                        (unsigned long long)s, strerror(errno));
                free(retryable);
                return ERR_INTERRUPTED;
            }

            mark_copied(s);
            record_read_success(1);
            progress = true;

            meta_save();
            meta_save_backup();
            display_status(0, 0);
        }
    }

    free(retryable);
    return ERR_OK;
}

/* ?? Initialize output file ?????????????????????????????????????????? */

static int init_output_file(void)
{
    struct stat st;
    if (stat(g_output_path, &st) == 0) {
        /* File exists - open it, do not truncate */
        g_fd_out = open(g_output_path, O_WRONLY);
        if (g_fd_out < 0) {
            fprintf(stderr, "Error: cannot open output file '%s': %s\n",
                    g_output_path, strerror(errno));
            return -1;
        }
        return 0;
    }

    /* Create new file */
    g_fd_out = open(g_output_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (g_fd_out < 0) {
        fprintf(stderr, "Error: cannot create output file '%s': %s\n",
                g_output_path, strerror(errno));
        return -1;
    }

    if (g_zero_init) {
        /* Fill with zeroes */
        fprintf(stderr, "Initializing output file with zeroes...\n");
        size_t bufsz = 1048576;
        void *zbuf = calloc(1, bufsz);
        if (!zbuf) {
            fprintf(stderr, "Error: out of memory\n");
            return -1;
        }

        uint64_t remaining = g_file_size;
        while (remaining > 0) {
            size_t chunk = remaining < bufsz ? (size_t)remaining : bufsz;
            ssize_t wr = write(g_fd_out, zbuf, chunk);
            if (wr < 0) {
                fprintf(stderr, "Error: writing zeroes to output: %s\n", strerror(errno));
                free(zbuf);
                return -1;
            }
            remaining -= wr;
        }
        free(zbuf);
        lseek(g_fd_out, 0, SEEK_SET);
    } else {
        /* Sparse file: just set the size */
        if (ftruncate(g_fd_out, (off_t)g_file_size) < 0) {
            fprintf(stderr, "Error: ftruncate on output: %s\n", strerror(errno));
            return -1;
        }
    }

    return 0;
}

/* ?? Usage ??????????????????????????????????????????????????????????? */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options] input_file output_file\n"
        "\n"
        "Options:\n"
        "  -z              Initialize output file with zeroes (no sparse file)\n"
        "  -R              Reset retry counts in metadata file\n"
        "  -d              Enable status display\n"
        "  -r <count>      Max retries per sector (default: %d)\n"
        "  -e <count>      Max total read errors per run (default: unlimited)\n"
        "  -E <count>      Max continuous read errors (default: unlimited)\n"
        "  -m <size>       Minimum area size to copy\n"
        "  -k <ratio>      Skip ratio (default: %.4f)\n"
        "  -s <size>       Sector size (default: %d)\n"
        "  -b <size>       Buffer size (default: 32M)\n"
        "  -f <size>       Max forward copy segment size (default: 4M)\n"
        "\n"
        "Sizes accept suffixes: k/m/g/t (SI) or K/M/G/T (binary).\n"
        "Decimals are allowed with suffixes, e.g. 1.5G.\n",
        argv0, DEFAULT_MAX_RETRIES, DEFAULT_SKIP_RATIO, DEFAULT_SECTOR_SIZE);
}

/* ?? Main ???????????????????????????????????????????????????????????? */

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "zRdr:e:E:m:k:s:b:f:h")) != -1) {
        switch (opt) {
        case 'z':
            g_zero_init = true;
            break;
        case 'R':
            g_reset_retries = true;
            break;
        case 'd':
            g_show_status = true;
            break;
        case 'r':
            if (parse_int_size(optarg, &g_max_retries) < 0 || g_max_retries < 0) {
                fprintf(stderr, "Error: invalid max retries: '%s'\n", optarg);
                return 1;
            }
            if (g_max_retries > STATUS_MAX_FAILS)
                g_max_retries = STATUS_MAX_FAILS;
            break;
        case 'e':
            if (parse_int_size(optarg, &g_max_total_errors) < 0) {
                fprintf(stderr, "Error: invalid max total errors: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'E':
            if (parse_int_size(optarg, &g_max_cont_errors) < 0) {
                fprintf(stderr, "Error: invalid max continuous errors: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'm':
            if (parse_size(optarg, &g_min_area_size) < 0) {
                fprintf(stderr, "Error: invalid min area size: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'k': {
            char *end;
            g_skip_ratio = strtod(optarg, &end);
            if (*end || g_skip_ratio < 0 || g_skip_ratio >= 1.0) {
                fprintf(stderr, "Error: invalid skip ratio: '%s' (must be 0..1)\n", optarg);
                return 1;
            }
            break;
        }
        case 's':
            if (parse_size(optarg, &g_sector_size) < 0 || g_sector_size == 0) {
                fprintf(stderr, "Error: invalid sector size: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'b':
            if (parse_size(optarg, &g_buffer_size) < 0 || g_buffer_size == 0) {
                fprintf(stderr, "Error: invalid buffer size: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'f':
            if (parse_size(optarg, &g_max_fwd_segment) < 0 || g_max_fwd_segment == 0) {
                fprintf(stderr, "Error: invalid max forward segment: '%s'\n", optarg);
                return 1;
            }
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (optind + 2 != argc) {
        usage(argv[0]);
        return 1;
    }

    g_input_path  = argv[optind];
    g_output_path = argv[optind + 1];

    /* Ensure buffer size is a multiple of sector size */
    if (g_buffer_size < g_sector_size)
        g_buffer_size = g_sector_size;
    g_buffer_size = (g_buffer_size / g_sector_size) * g_sector_size;

    /* Build metadata file paths */
    snprintf(g_meta_path, sizeof(g_meta_path), "%s.rescue", g_output_path);
    snprintf(g_meta_bak_path, sizeof(g_meta_bak_path), "%s.rescue.bak", g_output_path);

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open input file and determine size */
    g_fd_in = open(g_input_path, O_RDONLY);
    if (g_fd_in < 0) {
        fprintf(stderr, "Error: cannot open input file '%s': %s\n",
                g_input_path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(g_fd_in, &st) < 0) {
        fprintf(stderr, "Error: cannot stat input file '%s': %s\n",
                g_input_path, strerror(errno));
        close(g_fd_in);
        return 1;
    }

    if (S_ISREG(st.st_mode)) {
        g_file_size = (uint64_t)st.st_size;
    } else if (S_ISBLK(st.st_mode)) {
        /* Try to get block device size */
        off_t end = lseek(g_fd_in, 0, SEEK_END);
        if (end < 0) {
            fprintf(stderr, "Error: cannot determine size of block device '%s': %s\n",
                    g_input_path, strerror(errno));
            close(g_fd_in);
            return 1;
        }
        g_file_size = (uint64_t)end;
        lseek(g_fd_in, 0, SEEK_SET);
    } else {
        fprintf(stderr, "Error: input '%s' is neither a regular file nor a block device\n",
                g_input_path);
        close(g_fd_in);
        return 1;
    }

    if (g_file_size == 0) {
        fprintf(stderr, "Error: input file has zero size\n");
        close(g_fd_in);
        return 1;
    }

    g_sector_count = (g_file_size + g_sector_size - 1) / g_sector_size;

    char sz1[32], sz2[32], sz3[32];
    fprintf(stderr, "Input: %s (%s, %llu sectors of %s)\n",
            g_input_path,
            fmt_size(g_file_size, sz1, sizeof(sz1)),
            (unsigned long long)g_sector_count,
            fmt_size(g_sector_size, sz2, sizeof(sz2)));
    fprintf(stderr, "Output: %s\n", g_output_path);
    fprintf(stderr, "Metadata: %s\n", g_meta_path);
    fprintf(stderr, "Buffer: %s\n", fmt_size(g_buffer_size, sz3, sizeof(sz3)));

    /* Allocate status map */
    g_status_map = calloc(g_sector_count, 1);
    if (!g_status_map) {
        fprintf(stderr, "Error: cannot allocate status map for %llu sectors\n",
                (unsigned long long)g_sector_count);
        close(g_fd_in);
        return 1;
    }

    /* Allocate I/O buffer */
    g_io_buffer = malloc(g_buffer_size);
    if (!g_io_buffer) {
        fprintf(stderr, "Error: cannot allocate I/O buffer of %llu bytes\n",
                (unsigned long long)g_buffer_size);
        free(g_status_map);
        close(g_fd_in);
        return 1;
    }

    /* Try to load existing metadata */
    bool meta_loaded = false;
    if (meta_load(g_meta_path) == 0) {
        fprintf(stderr, "Loaded metadata from '%s'\n", g_meta_path);
        meta_loaded = true;
        g_first_pass = false;
    } else if (meta_load(g_meta_bak_path) == 0) {
        fprintf(stderr, "Loaded metadata from backup '%s'\n", g_meta_bak_path);
        meta_loaded = true;
        g_first_pass = false;
    }

    if (g_reset_retries && meta_loaded) {
        fprintf(stderr, "Resetting retry counts...\n");
        for (uint64_t i = 0; i < g_sector_count; i++) {
            if (g_status_map[i] >= 2)
                g_status_map[i] = STATUS_NOT_COPIED;
        }
    }

    recount_stats();

    if (meta_loaded) {
        fprintf(stderr, "Status: %llu copied, %llu untried, %llu failed\n",
                (unsigned long long)g_sectors_copied,
                (unsigned long long)g_sectors_untried,
                (unsigned long long)g_sectors_failed);
    }

    /* Open/create output file */
    if (init_output_file() < 0) {
        free(g_io_buffer);
        free(g_status_map);
        close(g_fd_in);
        return 1;
    }

    /* Save initial metadata */
    if (meta_save() < 0) {
        free(g_io_buffer);
        free(g_status_map);
        close(g_fd_in);
        close(g_fd_out);
        return 1;
    }
    if (!meta_loaded) {
        meta_save_backup();
    }

    /* Set up display */
    if (g_show_status) {
        get_terminal_size();
        /* Clear screen */
        printf("\033[2J\033[H");
        fflush(stdout);
    }

    gettimeofday(&g_start_time, NULL);

    /* ?? Execute recovery phases ??? */

    err_check_t result;

    fprintf(stderr, "Phase 1: Fast copy of large ranges...\n");
    display_status(0, 0);
    result = phase1();

    if (result == ERR_OK) {
        fprintf(stderr, "Phase 2: Split-and-copy of remaining ranges...\n");
        display_status(0, 0);
        result = phase2();
    }

    if (result == ERR_OK) {
        fprintf(stderr, "Phase 3: Retry failed sectors...\n");
        display_status(0, 0);
        result = phase3();
    }

    /* Final save */
    meta_save();

    /* Final statistics */
    recount_stats();

    if (g_show_status) {
        /* Move below the status display */
        printf("\033[%d;1H\n", g_term_rows + 1);
    }

    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    double elapsed = (end_time.tv_sec - g_start_time.tv_sec)
                   + (end_time.tv_usec - g_start_time.tv_usec) / 1e6;

    char s1[32], s2[32];
    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "Copied:   %llu / %llu sectors (%s / %s)\n",
            (unsigned long long)g_sectors_copied,
            (unsigned long long)g_sector_count,
            fmt_size(g_sectors_copied * g_sector_size, s1, sizeof(s1)),
            fmt_size(g_file_size, s2, sizeof(s2)));
    fprintf(stderr, "Untried:  %llu sectors\n", (unsigned long long)g_sectors_untried);
    fprintf(stderr, "Failed:   %llu sectors\n", (unsigned long long)g_sectors_failed);
    fprintf(stderr, "Errors:   %d total, %d continuous\n", g_total_errors, g_cont_errors);
    fprintf(stderr, "Elapsed:  %.1f seconds\n", elapsed);
    if (elapsed > 0.01)
        fprintf(stderr, "Speed:    %s/s\n",
                fmt_size((uint64_t)(g_bytes_read_ok / elapsed), s1, sizeof(s1)));

    if (g_sectors_copied == g_sector_count) {
        fprintf(stderr, "Result:   COMPLETE - all sectors copied successfully.\n");
    } else if (result == ERR_CONT_LIMIT) {
        fprintf(stderr, "Result:   ABORTED - continuous error limit reached.\n");
        fprintf(stderr, "          Consider resetting the device and restoring\n");
        fprintf(stderr, "          the backup status file '%s'\n", g_meta_bak_path);
        fprintf(stderr, "          Then re-run with -R to reset retry counts.\n");
    } else if (result == ERR_TOTAL_LIMIT) {
        fprintf(stderr, "Result:   ABORTED - total error limit reached.\n");
    } else if (result == ERR_INTERRUPTED) {
        fprintf(stderr, "Result:   INTERRUPTED - progress saved.\n");
    } else {
        fprintf(stderr, "Result:   INCOMPLETE - %llu sectors could not be read.\n",
                (unsigned long long)(g_sectors_untried + g_sectors_failed));
    }

    /* Cleanup */
    free(g_io_buffer);
    free(g_status_map);
    close(g_fd_in);
    close(g_fd_out);

    return (g_sectors_copied == g_sector_count) ? 0 : 1;
}
