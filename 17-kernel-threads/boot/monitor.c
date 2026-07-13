#include "monitor.h"
#include "keyboard.h"
#include "kheap.h"
#include "phys_mem.h"
#include "thread.h"
#include "timer.h"

#define LINE_MAX 128U

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) { return 0; }
    }
    return 1;
}

static u32 str_to_u32(const char *s)
{
    u32 n = 0U;

    while (*s >= '0' && *s <= '9') {
        n = n * 10U + (u32)(*s - '0');
        s++;
    }
    return n;
}

static void readline(char *buf, u32 size)
{
    u32 pos = 0U;
    char ch;

    while (1) {
        ch = keyboard_getchar();

        if (ch == '\n') {
            console_printf("\n");
            break;
        }

        if (ch == '\b') {
            if (pos > 0U) {
                pos--;
                console_printf("\b \b");
            }
            continue;
        }

        if (pos < size - 1U) {
            buf[pos++] = ch;
            console_printf("%c", ch);
        }
    }

    buf[pos] = '\0';
}

static void cmd_help(void)
{
    console_set_color(0x0BU);
    console_printf("  help          this message\n");
    console_printf("  clear         clear screen\n");
    console_printf("  mem           physical and heap memory info\n");
    console_printf("  ticks         current PIT tick count\n");
    console_printf("  sleep <ms>    busy-wait N milliseconds\n");
    console_printf("  echo <text>   print text\n");
    console_printf("  threads       list kernel threads\n");
}

static void cmd_threads(void)
{
    const thread_t *head = thread_list();
    const thread_t *t    = head;
    const thread_t *cur  = thread_current();

    do {
        console_set_color(t->state == THREAD_DEAD
                          ? 0x08U
                          : (t == cur ? 0x0AU : 0x07U));
        console_printf("  [%u] %s%s\n",
                       t->id,
                       t->state == THREAD_DEAD ? "dead" : "running",
                       t == cur ? " *" : "");
        t = t->next;
    } while (t != head);
}

static void cmd_clear(void)
{
    console_clear();
}

static void cmd_mem(void)
{
    console_set_color(0x0AU);
    console_printf("phys free : %u pages (%uMB)\n",
                   phys_mem_free_count(),
                   phys_mem_free_count() / 256U);
    console_printf("heap used : %u bytes\n", kheap_used());
}

static void cmd_ticks(void)
{
    console_set_color(0x0EU);
    console_printf("ticks: %u\n", timer_ticks());
}

static void cmd_sleep(const char *arg)
{
    u32 ms = str_to_u32(arg);
    u32 t0 = timer_ticks();

    console_set_color(0x07U);
    console_printf("sleeping %ums...\n", ms);
    timer_sleep(ms);
    console_set_color(0x0AU);
    console_printf("done (delta=%u ticks)\n", timer_ticks() - t0);
}

static void cmd_echo(const char *arg)
{
    console_set_color(0x0FU);
    console_printf("%s\n", arg);
}

static void dispatch(const char *line)
{
    if (str_eq(line, "help")) {
        cmd_help();
    } else if (str_eq(line, "clear")) {
        cmd_clear();
    } else if (str_eq(line, "mem")) {
        cmd_mem();
    } else if (str_eq(line, "ticks")) {
        cmd_ticks();
    } else if (str_starts(line, "sleep ")) {
        cmd_sleep(line + 6U);
    } else if (str_starts(line, "echo ")) {
        cmd_echo(line + 5U);
    } else if (str_eq(line, "threads")) {
        cmd_threads();
    } else if (line[0] != '\0') {
        console_set_color(0x0CU);
        console_printf("unknown: %s\n", line);
    }
}

void monitor_run(void)
{
    char line[LINE_MAX];

    console_set_color(0x0BU);
    console_printf("kernel monitor ready -- type 'help' for commands\n");

    for (;;) {
        console_set_color(0x0AU);
        console_printf("monitor> ");
        console_set_color(0x0FU);
        readline(line, LINE_MAX);
        dispatch(line);
    }
}
