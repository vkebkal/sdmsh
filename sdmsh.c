#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <err.h>
#include <math.h>
#include <limits.h> /* SHRT_MAX */
#include <endian.h>
#include <assert.h>
#include <getopt.h>
#include <libgen.h> /* basename() */

#include <readline/readline.h>

#include <sdm.h>
#include <shell.h>
#include <sdmsh_commands.h>

#define SDM_PORT    4200

#define SDMSH_UPDATE_RX_STATE_SAMPLES 0x1ffff

enum {
    FLAG_SHOW_HELP     = 0x01,
    FLAG_SEND_STOP     = 0x02,
    FLAG_IGNORE_ERRORS = 0x04,
};

struct shell_config shell_config;
sdm_session_t *sdm_session;

void show_usage_and_die(int err, char *progname) {
    printf("Usage: %s [OPTIONS] IP/NUM [command; [command;] ...]\n"
           "Mandatory argument IP of EvoLogics S2C Software Defined Modem. Or NUM is 192.168.0.NUM.\n"
           "\n"
           "  -f, --file=FILENAME        Run commands from FILENAME. Can be apply multiply time.\n"
           "  -e, --expression=\"cmd\"   Run commands separeted by ';'. Can be apply multiply time.\n"
           "  -x, --ignore-errors        If commands running from FILE, do not exit on error reply of SDM modem, \n"
           "  -h, --help                 Display this help and exit\n"
           "  -p, --port=PORT            Set TCP PORT for connecting the SDM modem. Default is %d\n"
           "  -s, --stop                 Send SMD STOP at start\n"
           "  -v, --verbose[=log-level]  Set log level. Without parameter enable debug logging\n"
           "\n"
           "Examples:\n"
           "\n"
           "# Connect to 192.168.0.127 port 4200. Enable debug logging\n"
           "$ %s 127 -v\n"
           "\n"
           "# Connect to 10.0.0.10 to port 4201. Send SDM 'STOP' at start\n"
           "$ %s -sp 4201 10.0.0.10\n"
           "\n"
           "# Connect to 131 port 4200 and run commands from file 'rx.sdmsh'\n"
           "$ %s 131 -f rx.sdmsh\n"
           "\n"
           "# Run commands from command line\n"
           "$ %s 127 -e 'config 350 0 3; ref examples/1834_polychirp_re_down.dat; rx 2048 rcv'\n"
           "\n"
           "# Run two script with delay between them\n"
           "$ %s 127 -f rx.sdmsh -e 'usleep 1000000' -f tx.sdmsh\n"
               , progname, SDM_PORT, progname, progname, progname, progname, progname);
    exit(err);
}

struct option long_options[] = {
    {"file",          required_argument, 0, 'f'},
    {"expression",    required_argument, 0, 'e'},
    {"help",          no_argument,       0, 'h'},
    {"port",          required_argument, 0, 'p'},
    {"stop",          no_argument,       0, 's'},
    {"verbose",       optional_argument, 0, 'v'},
    {"ignore-errors", no_argument,       0, 'x'},
};
int option_index = 0;

static void sdmsh_signal_event_hook(int signo)
{
    switch (signo) {
        case SIGINT:
            if (sdm_session->state == SDM_STATE_RX) {
                rl_clear_visible_line();
                sdm_cmd(sdm_session, SDM_CMD_STOP);
            }
            break;

        default:
            break;
    }
}

void sdmsh_update_promt_state(sdm_session_t *ss, char *host)
{
    static int old_state = -1;
    static unsigned long data_len = 0;

    if(old_state == -1) {
        old_state = ss->state;
        return;
    }

    if (old_state == ss->state &&
      !(ss->state == SDM_STATE_RX && ss->data_len - data_len >= SDMSH_UPDATE_RX_STATE_SAMPLES))
        return;

    if (ss->state == SDM_STATE_RX) {
        rl_message("%s:rx[%d]> ", strrchr(host, '.') + 1, ss->data_len / 2);
        data_len = ss->data_len;
    } else {
        rl_clear_message();
        data_len = 0;
    }

    old_state = ss->state;
}

int main(int argc, char *argv[])
{
    char *progname, *host;
    int rc = 0;
    int len;
    char buf[BUFSIZE];

    int port = SDM_PORT;
    int opt, flags = 0;

    progname = basename(argv[0]);
    shell_input_init(&shell_config);

    /* check command line arguments */
    while ((opt = getopt_long(argc, argv, "hv::sf:e:p:x", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h': flags |= FLAG_SHOW_HELP;   break;
            case 's': flags |= FLAG_SEND_STOP;   break;
            case 'x': flags |= FLAG_IGNORE_ERRORS;  break;
            case 'p':
                      if (optarg == NULL)
                          show_usage_and_die(2, progname);
                      port = atoi(optarg);
                      break;

            case 'e':
                      if (optarg == NULL)
                          show_usage_and_die(2, progname);

                      rc = shell_input_add(&shell_config, SHELL_INPUT_TYPE_STRING, optarg);
                      if (rc == -1)
                          err(2, "Error open file /dev/zero\n");
                      else if (rc == -2)
                          errx(1, "Too many inputs. Maximum %d", SHELL_MAX_INPUT);

                      break;

            case 'f':
                      if (optarg == NULL)
                          show_usage_and_die(2, progname);

                      /* POSIX standard: read script from stdin if file name '-' */
                      if (optarg[0] == '-' && optarg[1] == 0)
                          rc = shell_input_add(&shell_config, SHELL_INPUT_TYPE_STDIO);
                      else
                          rc = shell_input_add(&shell_config, SHELL_INPUT_TYPE_FILE, optarg);

                      if (rc == -1)
                          err(2, "Error open script file \"%s\": "
                                  , optarg[0] == '-' && optarg[1] == 0 ? "stdin" : optarg);
                      else if (rc == -2)
                          errx(1, "Too many inputs. Maximum %d", SHELL_MAX_INPUT);

                      break;

            case 'v': {
                      char *endptr;

                      if (optarg == NULL) {
                          log_level |= DEBUG_LOG;
                          break;
                      }

                      log_level = strtoul(optarg, &endptr, 0);
                      if ((errno == ERANGE && log_level == ULONG_MAX)
                              || (errno != 0 && log_level == 0)
                              || (errno == 0 && *endptr != 0)
                              || (optarg == endptr)) {
                          fprintf (stderr, "log-level: must be a digit\n");
                          return 1;
                      }

                      break;
            }
            case '?':
                      return 2;
            default:
                      show_usage_and_die(2, progname);
        }
    }

    /* As argument we need at least IP/NUM */
    if (optind >= argc)
        show_usage_and_die(2, progname);

    /* Mandatory argument IP/NUM */
    if (optind++ < argc) {
        static char host_[14];
        char *endptr;
        long num = strtol(argv[optind - 1], &endptr, 10);

        if (endptr != NULL && *endptr == 0 && num >= 1 && num <= 254) {
            sprintf (host_, "192.168.0.%lu", num);
            host = host_;
        } else {
            host = argv[optind - 1];
        }
    }

    if (flags & FLAG_SHOW_HELP)
        show_usage_and_die(0, progname);

    logger(DEBUG_LOG, "Connect to %s:%d\n", host, port);
    sdm_session = sdm_connect(host, port);

    if (sdm_session == NULL)
        err(1, "sdm_connect(\"%s:%d\"): ", host, port);

    if (flags & FLAG_SEND_STOP)
        sdm_cmd(sdm_session, SDM_CMD_STOP);

    if (optind < argc)
            show_usage_and_die(2, progname);

    shell_config.progname = progname;
    shell_config.cookie   = sdm_session;
    shell_config.commands = commands;
    shell_config.drivers  = drivers;
    shell_config.signal_event_hook = sdmsh_signal_event_hook;
    shell_init(&shell_config);

    shell_update_prompt(&shell_config, "%s> ", strrchr(host, '.') + 1);
    for (;;) {
        fd_set rfds;
        int maxfd = sdm_session->sockfd + 1;
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;
        FD_ZERO(&rfds);
        FD_SET(sdm_session->sockfd, &rfds);
        rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0) {
            len = read(sdm_session->sockfd, buf, sizeof(buf));
            if (len > 0) continue;
        }
        break;
    }
    
    for (;;) {
        static fd_set rfds;
        static struct timeval tv;
        static int maxfd;

        FD_ZERO(&rfds);
        FD_SET(sdm_session->sockfd, &rfds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        if (sdm_session->state == SDM_STATE_INIT) {
            /* In init state we want flush all data what left in modem from last session.
             * So we did't read user input till hit timeout.
             */
            tv.tv_sec  = 0;
            tv.tv_usec = 10;
            maxfd = sdm_session->sockfd;
        } else if (!is_interactive_mode(&shell_config) &&
                   (sdm_session->state == SDM_STATE_WAIT_REPLY ||
                    sdm_session->state == SDM_STATE_RX)) {
            /* If we running script, we need to wait for reply before run next command */
            maxfd = sdm_session->sockfd;
        } else if (!is_interactive_mode(&shell_config) && !shell_config.input) {
            break;
        } else {
            FD_SET(fileno(shell_config.input), &rfds);
            maxfd = (sdm_session->sockfd > fileno(shell_config.input)) ? sdm_session->sockfd : fileno(shell_config.input);
        }
        sdmsh_update_promt_state(sdm_session, host);

        rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (rc == -1 && errno == EINTR)
            continue;

        if (rc == -1)
            err(1, "select()");
        /* timeout */
        if (!rc) {
            if (sdm_session->state == SDM_STATE_INIT)
                sdm_session->state = SDM_STATE_IDLE;
            continue;
        }

        if (shell_config.input && FD_ISSET(fileno(shell_config.input), &rfds)) {
            rl_callback_read_char();
            rc = shell_handle(&shell_config);

            if(rc < 0) {
                /* shell want to quit */
                if (!is_interactive_mode(&shell_config)) {

                    rc = rc == SHELL_EOF ? 0 : rc;
                    if (rc < 0) { /* FIXME: if (sdm_session->state == SDM_STATE_IDLE)???  */
                        break;
                    } else if (sdm_session->state == SDM_STATE_IDLE)
                        break;
                } else if (rc == SHELL_EOF) {
                    rc = 0;
                    break;
                }
            }
        }

        if (FD_ISSET(sdm_session->sockfd, &rfds)) {
            len = read(sdm_session->sockfd, buf, sizeof(buf));

            if (len == 0)
                break;

            if (len < 0)
              err(1, "read(): ");

            do {
                rc = sdm_handle_rx_data(sdm_session, buf, len);
                if (len && (sdm_session->rx_data_len == 0 || rc == 0))
                    shell_forced_update_display(&shell_config);
                len = 0;
            } while (rc > 0);

            if (rc < 0)
                if (!is_interactive_mode(&shell_config) && !(flags & FLAG_IGNORE_ERRORS))
                    break;
        }
    }

    shell_deinit(&shell_config);
    sdm_close(sdm_session);

    return rc < 0 ? -rc : rc;
}
