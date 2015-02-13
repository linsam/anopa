
#include <unistd.h>
#include <errno.h>
#include <skalibs/bytestr.h>
#include <skalibs/tai.h>
#include <skalibs/error.h>
#include <skalibs/strerr2.h>
#include <s6/s6-supervise.h>
#include <s6/ftrigr.h>
#include <anopa/service.h>
#include <anopa/err.h>
#include "service_internal.h"

int
_exec_longrun (int si, aa_mode mode)
{
    aa_service *s = aa_service (si);
    s6_svstatus_t st6 = S6_SVSTATUS_ZERO;
    int l_sn = strlen (aa_service_name (s));
    char fifodir[l_sn + 1 + sizeof (S6_SUPERVISE_EVENTDIR)];
    tain_t deadline;
    int is_start = (mode == AA_MODE_START) ? 1 : 0;
    char *event = (is_start) ? "u" : "d";

    byte_copy (fifodir, l_sn, aa_service_name (s));
    fifodir[l_sn] = '/';
    byte_copy (fifodir + l_sn + 1, sizeof (S6_SUPERVISE_EVENTDIR), S6_SUPERVISE_EVENTDIR);

    tain_addsec_g (&deadline, 1);
    s->ft_id = ftrigr_subscribe_g (&_aa_ft, fifodir, event, 0, &deadline);
    if (s->ft_id == 0)
    {
        /* this could happen e.g. if the servicedir isn't in scandir, if
         * something failed during aa-enable for example */

        s->st.event = (is_start) ? AA_EVT_STARTING_FAILED : AA_EVT_STOPPING_FAILED;
        s->st.code = ERR_S6;
        tain_copynow (&s->st.stamp);
        aa_service_status_set_msg (&s->st, "Failed to subscribe to eventdir");
        if (aa_service_status_write (&s->st, aa_service_name (s)) < 0)
            strerr_warnwu2sys ("write service status file for ", aa_service_name (s));

        if (_exec_cb)
            _exec_cb (si, s->st.event, 0);
        return -1;
    }

    if (s6_svstatus_read (aa_service_name (s), &st6)
            && ((is_start && !!st6.pid) || (!is_start && !st6.pid)))
    {
        tain_now_g ();

        /* already there; unsubcribe */
        ftrigr_unsubscribe_g (&_aa_ft, s->ft_id, &deadline);
        s->ft_id = 0;

        /* make sure our status is correct, and timestamped before s6 */
        s->st.event = (is_start) ? AA_EVT_STARTING : AA_EVT_STOPPING;
        tain_addsec (&s->st.stamp, &st6.stamp, -1);
        aa_service_status_set_msg (&s->st, "");
        if (aa_service_status_write (&s->st, aa_service_name (s)) < 0)
            strerr_warnwu2sys ("write service status file for ", aa_service_name (s));

        if (_exec_cb)
            _exec_cb (si, (is_start) ? -ERR_ALREADY_UP: -ERR_NOT_UP, 0);

        /* this was not a failure, but we return -1 to trigger a
         * aa_scan_mainlist() anyways, since the service changed state */
        return -1;
    }
    tain_now_g ();

    s->st.event = (is_start) ? AA_EVT_STARTING : AA_EVT_STOPPING;
    tain_copynow (&s->st.stamp);
    aa_service_status_set_msg (&s->st, "");
    if (aa_service_status_write (&s->st, aa_service_name (s)) < 0)
    {
        s->st.event = (is_start) ? AA_EVT_STARTING_FAILED : AA_EVT_STOPPING_FAILED;
        s->st.code = ERR_WRITE_STATUS;
        aa_service_status_set_msg (&s->st, error_str (errno));

        if (_exec_cb)
            _exec_cb (si, s->st.event, 0);
        return -1;
    }

    if (mode == AA_MODE_STOP_ALL)
    {
        char dir[l_sn + 5 + sizeof (S6_SUPERVISE_CTLDIR) + 8];

        byte_copy (dir, l_sn, aa_service_name (s));
        byte_copy (dir + l_sn, 13 + sizeof (S6_SUPERVISE_CTLDIR), "/log/" S6_SUPERVISE_CTLDIR "/control");

        /* ignore any error, starting with the fact that there might not be a
         * logger for this service */
        s6_svc_write (dir, "x", 1);
    }

    {
        char dir[l_sn + 1 + sizeof (S6_SUPERVISE_CTLDIR) + 8];
        int r;

        byte_copy (dir, l_sn, aa_service_name (s));
        byte_copy (dir + l_sn, 9 + sizeof (S6_SUPERVISE_CTLDIR), "/" S6_SUPERVISE_CTLDIR "/control");

        r = s6_svc_write (dir, (mode == AA_MODE_STOP_ALL) ? "dx" : event,
                (mode == AA_MODE_STOP_ALL) ? 2 : 1);
        if (r <= 0)
        {
            tain_addsec_g (&deadline, 1);
            ftrigr_unsubscribe_g (&_aa_ft, s->ft_id, &deadline);
            s->ft_id = 0;

            s->st.event = (is_start) ? AA_EVT_STARTING_FAILED : AA_EVT_STOPPING_FAILED;
            s->st.code = ERR_S6;
            tain_copynow (&s->st.stamp);
            aa_service_status_set_msg (&s->st, (r < 0)
                    ? "Failed to send command"
                    : "Supervisor not listenning");
            if (aa_service_status_write (&s->st, aa_service_name (s)) < 0)
                strerr_warnwu2sys ("write service status file for ", aa_service_name (s));

            if (_exec_cb)
                _exec_cb (si, s->st.event, 0);
            return -1;
        }
    }

    if (is_start)
    {
        char buf[l_sn + 6];

        byte_copy (buf, l_sn, aa_service_name (s));
        byte_copy (buf + l_sn, 6, "/down");

        unlink (buf);
    }

    if (_exec_cb)
        _exec_cb (si, s->st.event, 0);
    return 0;
}

int
aa_get_longrun_info (uint16 *id, char *event)
{
    static int i = -1;
    int r;

    if (i == -1)
    {
        r = ftrigr_update (&_aa_ft);
        if (r <= 0)
            return r;
        i = 0;
    }

    for ( ; i < genalloc_len (uint16, &_aa_ft.list); )
    {
        *id = genalloc_s (uint16, &_aa_ft.list)[i];
        r = ftrigr_check (&_aa_ft, *id, event);
        ++i;
        if (r != 0)
            return r;
    }

    i = -1;
    return 0;
}
