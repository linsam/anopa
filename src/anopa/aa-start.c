
#define _BSD_SOURCE

#include <locale.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <langinfo.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <fcntl.h>
#include <signal.h>
#include <skalibs/djbunix.h>
#include <skalibs/bytestr.h>
#include <skalibs/direntry.h>
#include <skalibs/genalloc.h>
#include <skalibs/strerr2.h>
#include <skalibs/error.h>
#include <skalibs/tai.h>
#include <skalibs/iopause.h>
#include <skalibs/djbunix.h>
#include <skalibs/uint16.h>
#include <skalibs/uint.h>
#include <anopa/common.h>
#include <anopa/err.h>
#include <anopa/init_repo.h>
#include <anopa/output.h>
#include <anopa/scan_dir.h>
#include <anopa/ga_int_list.h>
#include <anopa/service_status.h>
#include <anopa/service.h>
#include <anopa/progress.h>
#include <anopa/stats.h>
#include "start-stop.h"
#include "util.h"


#define ESSENTIAL_FILENAME      "essential"

static genalloc ga_depend = GENALLOC_ZERO;
static genalloc ga_skipped = GENALLOC_ZERO;
static genalloc ga_unknown = GENALLOC_ZERO;
static genalloc ga_io = GENALLOC_ZERO;
static int no_wants = 0;
static int rc = 0;

void
check_essential (int si)
{
    if (rc == 0)
    {
        struct stat st;
        const char *name = aa_service_name (aa_service (si));
        int l_name = strlen (name);
        char buf[l_name + 1 + sizeof (ESSENTIAL_FILENAME)];

        byte_copy (buf, l_name, name);
        byte_copy (buf + l_name, 1 + sizeof (ESSENTIAL_FILENAME), "/" ESSENTIAL_FILENAME);

        if (stat (buf, &st) < 0)
        {
            if (errno != ENOENT)
            {
                int e = errno;
                put_warn (name, "Failed to stat " ESSENTIAL_FILENAME ": ", 0);
                add_warn (error_str (e));
                end_warn ();
            }
        }
        else
            rc = 1;
    }
}

static void
load_fail_cb (int si, aa_lf lf, const char *name, int err)
{
    if (lf == AA_LOADFAIL_WANTS)
    {
        put_warn (aa_service_name (aa_service (si)), "Skipping wanted service ", 0);
        add_warn (name);
        add_warn (": ");
        add_warn (errmsg[err]);
        end_warn ();
        add_name_to_ga (name, &ga_skipped);
    }
}

static int
add_service (const char *name)
{
    int si = -1;
    int type;
    int r;

    type = aa_get_service (name, &si, 1);
    if (type < 0)
        r = type;
    else
        r = aa_mark_service (si, type == AA_SERVICE_FROM_MAIN, no_wants, load_fail_cb);
    if (r < 0)
    {
        if (r == -ERR_UNKNOWN)
        {
            put_err_service (name, ERR_UNKNOWN, 1);
            add_name_to_ga (name, &ga_unknown);
        }
        else if (r == -ERR_IO)
        {
            /* ERR_IO from aa_get_service() means we don't have a si (it is
             * actually set to the return value); but from aa_mark_service()
             * (e.g. to read "needs") then we do */
            if (si < 0)
            {
                put_err_service (name, ERR_IO, 1);
                add_name_to_ga (name, &ga_io);
            }
            else
            {
                int e = errno;

                put_err_service (name, ERR_IO, 0);
                add_err (": ");
                add_err (error_str (e));
                end_err ();

                genalloc_append (int, &ga_failed, &si);
                check_essential (si);
            }
        }
        else if (r == -ERR_ALREADY_UP)
        {
            put_title (1, name, errmsg[-r], 1);
            ++nb_already;
            r = 0;
        }
        else
        {
            aa_service *s = aa_service (si);
            const char *msg = aa_service_status_get_msg (&s->st);
            int has_msg;

            has_msg = s->st.event == AA_EVT_ERROR && s->st.code == -r && !!msg;
            put_err_service (name, -r, !has_msg);
            if (has_msg)
            {
                add_err (": ");
                add_err (msg);
                end_err ();
            }

            if (r == -ERR_DEPEND)
                genalloc_append (int, &ga_depend, &si);
            else
                genalloc_append (int, &ga_failed, &si);
            check_essential (si);
        }
    }
    else
        r = 0;

    return r;
}

static int
it_start (direntry *d, void *data)
{
    tain_now_g ();
    add_service (d->d_name);
    return 0;
}

static void
scan_cb (int si, int sni)
{
    put_err_service (aa_service_name (aa_service (si)), ERR_DEPEND, 0);
    add_err (": ");
    add_err (aa_service_name (aa_service (sni)));
    end_err ();
    genalloc_append (int, &ga_depend, &si);
    check_essential (si);
}

static void
dieusage (void)
{
    aa_die_usage ("[OPTION...] [service...]",
            " -D, --double-output           Enable double-output mode\n"
            " -r, --repodir DIR             Use DIR as repository directory\n"
            " -l, --listdir DIR             Use DIR to list services to start\n"
            " -w, --no-wants                Don't auto-start services from 'wants'\n"
            " -h, --help                    Show this help screen and exit\n"
            " -V, --version                 Show version information and exit\n"
            );
}

static void
close_fd (int fd)
{
    close_fd_for (fd, -1);
}

int
main (int argc, char * const argv[])
{
    PROG = "aa-start";
    const char *path_repo = "/run/services";
    const char *path_list = NULL;
    int mode_both = 0;
    int i;

    for (;;)
    {
        struct option longopts[] = {
            { "double-output",      no_argument,        NULL,   'D' },
            { "help",               no_argument,        NULL,   'h' },
            { "listdir",            required_argument,  NULL,   'l' },
            { "repodir",            required_argument,  NULL,   'r' },
            { "version",            no_argument,        NULL,   'V' },
            { "no-wants",           no_argument,        NULL,   'w' },
            { NULL, 0, 0, 0 }
        };
        int c;

        c = getopt_long (argc, argv, "Dhl:r:Vw", longopts, NULL);
        if (c == -1)
            break;
        switch (c)
        {
            case 'D':
                mode_both = 1;
                break;

            case 'l':
                unslash (optarg);
                path_list = optarg;
                break;

            case 'r':
                unslash (optarg);
                path_repo = optarg;
                break;

            case 'w':
                no_wants = 1;
                break;

            case 'V':
                aa_die_version ();

            case 'h':
            default:
                dieusage ();
        }
    }
    argc -= optind;
    argv += optind;

    aa_init_output (mode_both);
    cols = get_cols (1);
    is_utf8 = is_locale_utf8 ();

    if (!path_list && argc < 1)
        dieusage ();

    if (aa_init_repo (path_repo, AA_REPO_WRITE) < 0)
        strerr_diefu2sys (ERR_IO, "init repository ", path_repo);

    if (path_list)
    {
        stralloc sa = STRALLOC_ZERO;
        int r;

        stralloc_catb (&sa, path_list, strlen (path_list) + 1);
        r = aa_scan_dir (&sa, 1, it_start, NULL);
        stralloc_free (&sa);
        if (r < 0)
            strerr_diefu2sys (-r, "read list directory ", path_list);
    }

    tain_now_g ();

    for (i = 0; i < argc; ++i)
        add_service (argv[i]);

    mainloop (AA_MODE_START, scan_cb);

    aa_bs_noflush (AA_OUT, "\n");
    put_title (1, PROG, "Completed.", 1);
    aa_show_stat_nb (nb_already, "Already up", ANSI_HIGHLIGHT_GREEN_ON);
    aa_show_stat_nb (nb_done, "Started", ANSI_HIGHLIGHT_GREEN_ON);
    show_stat_service_names (&ga_failed, "Failed", ANSI_HIGHLIGHT_RED_ON);
    show_stat_service_names (&ga_depend, "Dependency failed", ANSI_HIGHLIGHT_RED_ON);
    aa_show_stat_names (aa_names.s, &ga_io, "I/O error", ANSI_HIGHLIGHT_RED_ON);
    aa_show_stat_names (aa_names.s, &ga_unknown, "Unknown", ANSI_HIGHLIGHT_RED_ON);
    aa_show_stat_names (aa_names.s, &ga_skipped, "Skipped", ANSI_HIGHLIGHT_YELLOW_ON);

    genalloc_free (int, &ga_failed);
    genalloc_free (int, &ga_depend);
    genalloc_free (int, &ga_unknown);
    genalloc_free (int, &ga_io);
    genalloc_free (int, &ga_skipped);
    genalloc_free (pid_t, &ga_pid);
    genalloc_free (int, &aa_tmp_list);
    genalloc_free (int, &aa_main_list);
    stralloc_free (&aa_names);
    genalloc_deepfree (struct progress, &ga_progress, free_progress);
    aa_free_services (close_fd);
    genalloc_free (iopause_fd, &ga_iop);
    return rc;
}
