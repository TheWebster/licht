
#include "config.h"
#include "licht.h"
#include "util.h"
#include <errno.h>
#include <grp.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>


#define LOCKDIR cfg_PREFIX cfg_TMPDIR cfg_PKG_NAME
static const char g_lockdir[] = LOCKDIR;
static const char g_socket[]  = LOCKDIR "/socket";

licht_cmd_s g_cmd = {0};
licht_context_s g_ctx = {0};

static void process_cmd();
static void cleanup(void);
void *do_change(void *data);
void *do_listen(void *data);

static void print_usage(char *call);
static void print_version();


int main(int argc, char *argv[])
{
    #define streq_val argv[1]
    if(argc == 1 || streq("-H") || streq("-h") || streq("--help")) {
        print_usage(argv[0]);
        return 0;
    }
    
    if(streq("-V") || streq("--version")) {
        print_version();
        return 0;
    }
    
    if(streq("get"))
        g_cmd.op = LICHT_OP_GET;
    else if(streq("set"))
        g_cmd.op = LICHT_OP_SET;
    else if(streq("add"))
        g_cmd.op = LICHT_OP_ADD;
    else if(streq("sub"))
        g_cmd.op = LICHT_OP_SUB;
    else if(streq("mul"))
        g_cmd.op = LICHT_OP_MUL;
    else {
        LICHT_ERR("Unknown command '%s'\n", argv[1]);
        return 1;
    }
    #undef streq_val
    
    if(g_cmd.op != LICHT_OP_GET) {
        if(argc < 3)
            return 1;
        
        char *endptr;
        g_cmd.value = strtof(argv[2], &endptr);
        if(endptr == argv[2]) {
            LICHT_ERR("'%s' is not a number.\n", argv[2]);
            return 1;
        }
    }
    // get config
    FILE *fconf = open_conf_file();
    if(NULL == fconf) {
        fputs("Could not open any config file.\n", stderr);
        return 1;
    }
    
    #define MAX_LINE_LEN 256
    char *key, *value;
    char line_buf[MAX_LINE_LEN];
    while(EOF != parse_line(fconf, line_buf, MAX_LINE_LEN, &key, &value)) {
        if('\0' == key[0] || NULL == value)
            continue;
    
        #define streq_val key
        if(streq("DEVICE")) {
            if(open_device(value, &g_ctx.br_fd, &g_ctx.max_fd))
                return 1;
        }
        else if(streq("SMOOTH_DURATION")) {
            char *endptr;
            int  val = strtol(value, &endptr, 10);
            if(*endptr == '\0')
                g_cmd.smooth_duration = val;
        }
        else if(streq("SMOOTH_INTERVAL")) {
            char *endptr;
            int  val = strtol(value, &endptr, 10);
            if(*endptr == '\0')
                g_cmd.smooth_interval = val;
        }
        #undef streq_val
    }
    
    fclose(fconf);
    
    // set gid to cfg_GROUP
    
    // try to grab lock directory
    if(0 == mkdir(g_lockdir, S_IRWXU|S_IRWXG)) {
        struct group *grp = getgrnam(cfg_GROUP);
        
        chown(g_lockdir, -1, grp->gr_gid);
        
        if(-1 == setup_socket_recv(g_socket)) 
            return 1;
        
        g_ctx.max     = read_atoi(g_ctx.max_fd);
        g_ctx.current = read_atoi(g_ctx.br_fd);
        g_ctx.target  = g_ctx.current;
        process_cmd();
        
        printf("%f\n", 100.0f / g_ctx.max * g_ctx.target );
        
        // fork off
        if(fork())
            return 0;
        
        fclose(stdin);
        fclose(stdout);
        fclose(stderr);
        
        atexit(&cleanup);
        
        if(g_ctx.current == g_ctx.target)
            return 0;
        
        // start worker thread
        if(errno = pthread_create(&g_ctx.worker, NULL, &do_change, NULL)) {
            printf("%s\n", strerror(errno));
            return 1;
        }
        
        // start listening thread
        if(errno = pthread_create(&g_ctx.listen, NULL, &do_listen, NULL)) {
            printf("%s\n", strerror(errno));
            return 1;
        }
        
        pthread_join(g_ctx.worker, NULL);
    }
    else if(errno == EEXIST) {
        int fd;
        char c;
        
        if(fd = setup_socket_conn(g_socket), fd == -1)
            return 1;
        
        write(fd, &g_cmd, sizeof(g_cmd));
        while(read(fd, &c, 1) > 0 && c != '\n')
            fputc(c, stdout);
        fputc('\n', stdout);
    }
    else {
        LICHT_ERR("Could not create '%s' -- %s\n", g_lockdir, strerror(errno));
        return 1;
    }
    
    return 0;
}


static void cleanup(void)
{
    destroy_socket(g_socket);
    rmdir(g_lockdir);
}


static void process_cmd()
{
    // process command
    switch(g_cmd.op) {
        case LICHT_OP_SET:
            g_ctx.target = (int)nearbyintf(g_cmd.value / 100.0f * g_ctx.max);
            break;
        
        case LICHT_OP_ADD:
            g_ctx.target += (int)nearbyintf(g_cmd.value / 100.0f * g_ctx.max);
            break;
        
        case LICHT_OP_SUB:
            g_ctx.target -= (int)nearbyintf(g_cmd.value / 100.0f * g_ctx.max);
            break;
        
        case LICHT_OP_MUL:
            g_ctx.target = (int)nearbyintf(g_cmd.value * g_ctx.target);
            break;
        
        case LICHT_OP_GET:
            break;
    }
    
    // cap target value
    g_ctx.target =         (g_ctx.target < 0) ?         0 : g_ctx.target;
    g_ctx.target = (g_ctx.target > g_ctx.max) ? g_ctx.max : g_ctx.target;
}


void *do_change(void *data)
{
  reset:
    g_ctx.reset = 0;
    
    float delta_t = g_cmd.smooth_duration;
    float step_t  = g_cmd.smooth_interval;
    float delta   = g_ctx.target - g_ctx.current;
    FILE  *br     = fdopen(g_ctx.br_fd, "w");
    
    set_timer();
    while(delta_t) {
        int step;
        
        // cap step against remaining delta_t
        step_t = fminf(step_t, delta_t);
        step   = nearbyintf(step_t / delta_t * delta);
        g_ctx.current += step;
        
        write_itoa(br, g_ctx.current);
        rewind(br);                                         
        
        delta_t -= step_t;
        delta   -= step;
        
        if(g_ctx.reset)
            goto reset;
        
        wait_timer(step_t);
    }
    
    return NULL;
}


void *do_listen(void *data)
{
    int fd;
    while(fd = socket_recv(), fd != -1) {
        licht_cmd_s cmd;
        
        if(read(fd, &cmd, sizeof(cmd)) < sizeof(cmd))
            continue;
        
        if(cmd.op != LICHT_OP_GET) {
            g_cmd = cmd;
            process_cmd();
            g_ctx.reset = 1;
        }
        dprintf(fd, "%f\n", 100.0f / g_ctx.max * g_ctx.target );
    }
    
    return NULL;
}
        
        
        


void print_usage(char *call)
{
    fprintf(stderr,
        "Usage: %s [OPTION]... [cmd] [value]\n"
        "Options:\n"
        "  -H, -h, --help         No idea, try it out.\n"
        "  -V, --version          Print version info and quit.\n\n"
        "Commands:\n"
        "  get   Get current brightness.\n"
        "  set   Set brightness to <value>.\n"
        "  add   Add <value> to brightness.\n"
        "  sub   Subtract <value> from brightness.\n"
        "  mul   Multiply current brightness with <value>\n", call);
}


void print_version()
{
    fputs("licht Version "cfg_VERSION"\n"
          "Licence GPLv3\n"
          "Written by Christian Weber\n", stderr);
}
    