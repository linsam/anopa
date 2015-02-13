
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h> /* rename() */
#include <skalibs/bytestr.h>
#include <skalibs/djbunix.h>
#include <skalibs/direntry.h>
#include <skalibs/skamisc.h>
#include <anopa/enable_service.h>
#include <anopa/err.h>

static int
copy_from_source (const char        *name,
                  int                len,
                  aa_warn_fn         warn_fn,
                  aa_enable_flags    flags,
                  aa_auto_enable_cb  ae_cb);
static int
copy_dir (const char        *src,
          const char        *dst,
          mode_t             mode,
          int                depth,
          aa_warn_fn         warn_fn,
          aa_enable_flags    flags,
          aa_auto_enable_cb  ae_cb,
          const char        *instance);


static int
is_valid_service_name (const char *name, int len)
{
    if (len <= 0)
        return 0;
    if (name[0] == '@' || name[len - 1] == '@')
        return 0;
    if (byte_chr (name, len, '/') < len)
        return 0;
    return 1;
}

static int
copy_file (const char *src, const char *dst, mode_t mode)
{
    int fd_src;
    int fd_dst;

    fd_src = open_readb (src);
    if (fd_src < 0)
        return -1;

    fd_dst = open3 (dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd_dst < 0)
    {
        int e = errno;
        fd_close (fd_src);
        errno = e;
        return -1;
    }

    if (fd_cat (fd_src, fd_dst) < 0)
    {
        int e = errno;
        fd_close (fd_src);
        fd_close (fd_dst);
        errno = e;
        return -1;
    }

    fd_close (fd_src);
    fd_close (fd_dst);
    return 0;
}

static int
copy_log (const char *name, const char *cfg, mode_t mode, aa_warn_fn warn_fn)
{
    int fd;
    int r;
    int e;

    /* get current dir (repo) so we can come back */
    fd = open_read (".");
    if (fd < 0)
        return fd;

    /* and temporarily go into the servicedir */
    r = chdir (name);
    if (r < 0)
    {
        e = errno;
        fd_close (fd);
        errno = e;
        return r;
    }

    /* this is a logger, so there's no autoenable of any kind; hence we can use
     * 0 for flags (don't process it as a servicedir either, since it doesn't
     * apply) and not bother with a callback */
    r = copy_from_source ("log", 3, warn_fn, 0, NULL);

    if (r >= 0 && cfg)
        r = copy_dir (cfg, "log", mode, 0, warn_fn, 0, NULL, NULL);

    e = errno;
    fd_chdir (fd);
    fd_close (fd);
    errno = e;

    return r;
}

static int
copy_dir (const char        *src,
          const char        *dst,
          mode_t             mode,
          int                depth,
          aa_warn_fn         warn_fn,
          aa_enable_flags    flags,
          aa_auto_enable_cb  ae_cb,
          const char        *instance)
{
    unsigned int l_satmp = satmp.len;
    unsigned int l_max = strlen (AA_SCANDIR_DIRNAME);
    DIR *dir;
    struct stat st;
    struct {
        unsigned int run   : 1;
        unsigned int down  : 1;
        unsigned int began : 1;
    } has = { .run = 0, .down = 0, .began = 0 };

    dir = opendir (src);
    if (!dir)
        return -ERR_IO;

    if (depth == 0 && (flags & (_AA_FLAG_IS_SERVICEDIR | AA_FLAG_SKIP_DOWN))
            == (_AA_FLAG_IS_SERVICEDIR | AA_FLAG_SKIP_DOWN))
        /* treat as if there'e one, so don't create it */
        has.down = 1;

    errno = 0;
    for (;;)
    {
        direntry *d;
        unsigned int len;

        d = readdir (dir);
        if (!d)
            break;
        if (d->d_name[0] == '.'
                && (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0')))
            continue;
        len = strlen (d->d_name);
        if (len > l_max)
            l_max = len;
        if (!stralloc_catb (&satmp, d->d_name, len + 1))
            break;
        if (depth == 0 && (flags & _AA_FLAG_IS_SERVICEDIR))
        {
            if (!has.run && str_equal (d->d_name, "run"))
                has.run = 1;
            if (!has.down && str_equal (d->d_name, "down"))
                has.down = 1;
        }
    }
    if (errno)
    {
        int e = errno;
        dir_close (dir);
        errno = e;
        goto err;
    }
    dir_close (dir);

    if (mkdir (dst, S_IRWXU) < 0)
    {
        if (errno != EEXIST || stat (dst, &st) < 0)
            goto err;
        else if (!S_ISDIR (st.st_mode))
        {
            errno = ENOTDIR;
            goto err;
        }
        else if (flags & _AA_FLAG_IS_SERVICEDIR)
        {
            errno = EEXIST;
            goto err;
        }
    }

    if (flags & _AA_FLAG_IS_SERVICEDIR)
    {
        has.began = 1;
        flags &= ~_AA_FLAG_IS_SERVICEDIR;
    }

    {
        unsigned int l_inst = (instance) ? strlen (instance) : 0;
        unsigned int l_src = strlen (src);
        unsigned int l_dst = strlen (dst);
        unsigned int i = l_satmp;
        char buf_src[l_src + 1 + l_max + 1];
        char buf_dst[l_dst + 1 + l_max + l_inst + 1];

        byte_copy (buf_src, l_src, src);
        buf_src[l_src] = '/';
        byte_copy (buf_dst, l_dst, dst);
        buf_dst[l_dst] = '/';

        while (i < satmp.len)
        {
            unsigned int len;
            int r;

            len = strlen (satmp.s + i);
            byte_copy (buf_src + l_src + 1, len + 1, satmp.s + i);
            byte_copy (buf_dst + l_dst + 1, len + 1, satmp.s + i);

            if (stat (buf_src, &st) < 0)
            {
                warn_fn (buf_src, errno);
                goto err;
            }

            if (S_ISREG (st.st_mode))
            {
                if (has.began && depth == 0 && str_equal (satmp.s + i, "log"))
                {
                    r = copy_log (dst, NULL, 0, warn_fn);
                    st.st_mode = 0755;
                }
                else
                {
                    /* for any file in one of the 4 special places that ends
                     * with a '@' we append our instance name */
                    if (depth == 1 && instance && (flags & _AA_FLAG_IS_1OF4)
                            && satmp.s[i + len - 1] == '@')
                        byte_copy (buf_dst + l_dst + 1 + len, l_inst + 1, instance);
                    r = copy_file (buf_src, buf_dst, st.st_mode);
                    if (depth == 1 && r == 0 && ae_cb)
                    {
                        if ((flags & (AA_FLAG_AUTO_ENABLE_NEEDS | _AA_FLAG_IS_NEEDS))
                                == (AA_FLAG_AUTO_ENABLE_NEEDS | _AA_FLAG_IS_NEEDS))
                            ae_cb (buf_dst + l_dst + 1, AA_FLAG_AUTO_ENABLE_NEEDS);
                        else if ((flags & (AA_FLAG_AUTO_ENABLE_WANTS | _AA_FLAG_IS_WANTS))
                                == (AA_FLAG_AUTO_ENABLE_WANTS | _AA_FLAG_IS_WANTS))
                            ae_cb (buf_dst + l_dst + 1, AA_FLAG_AUTO_ENABLE_WANTS);
                    }
                }
            }
            else if (S_ISDIR (st.st_mode))
            {
                if (has.began && depth == 0 && str_equal (satmp.s + i, "log"))
                    r = copy_log (dst, buf_src, st.st_mode, warn_fn);
                else
                {
                    /* use depth because this is also enabled for the config part */
                    if (depth == 0)
                    {
                        /* flag to enable auto-rename of files above */
                        if (str_equal (satmp.s + i, "needs"))
                            flags |= _AA_FLAG_IS_NEEDS;
                        else if (str_equal (satmp.s + i, "wants"))
                            flags |= _AA_FLAG_IS_WANTS;
                        else if (str_equal (satmp.s + i, "before")
                                || str_equal (satmp.s + i, "after"))
                            flags |= _AA_FLAG_IS_BEF_AFT;
                    }
                    r = copy_dir (buf_src, buf_dst, st.st_mode, depth + 1,
                            warn_fn, flags, ae_cb, instance);
                    if (depth == 0)
                        flags &= ~_AA_FLAG_IS_1OF4;
                }
            }
            else if (S_ISFIFO (st.st_mode))
                r = mkfifo (buf_dst, st.st_mode);
            else if (S_ISLNK (st.st_mode))
            {
                unsigned int l_tmp = satmp.len;

                if ((sareadlink (&satmp, buf_src) < 0) || !stralloc_0 (&satmp))
                    r = -1;
                else
                    r = symlink (satmp.s + l_tmp, buf_dst);

                satmp.len = l_tmp;
            }
            else if (S_ISCHR (st.st_mode) || S_ISBLK (st.st_mode) || S_ISSOCK (st.st_mode))
                r = mknod (buf_dst, st.st_mode, st.st_rdev);
            else
            {
                errno = EOPNOTSUPP;
                r = -1;
            }

            if (r >= 0)
                r = lchown (buf_dst, st.st_uid, st.st_gid);
            if (r >= 0 && !S_ISLNK (st.st_mode) && !S_ISDIR (st.st_mode))
                r = chmod (buf_dst, st.st_mode);

            if (r < 0)
            {
                warn_fn (buf_src, errno);
                goto err;
            }

            i += len + 1;
        }

        if (has.run)
        {
            if (!has.down)
            {
                char buf[l_dst + 1 + strlen ("down") + 1];
                int fd;

                byte_copy (buf, l_dst, dst);
                buf[l_dst] = '/';
                byte_copy (buf + l_dst + 1, 5, "down");

                fd = open_create (buf);
                if (fd < 0)
                {
                    warn_fn (buf, errno);
                    goto err;
                }
                else
                    fd_close (fd);
            }

            {
                char buf_lnk[3 + l_dst + 1];
                char buf_dst[sizeof (AA_SCANDIR_DIRNAME) + l_dst + 1];

                byte_copy (buf_lnk, 3, "../");
                byte_copy (buf_lnk + 3, l_dst + 1, dst);

                byte_copy (buf_dst, sizeof (AA_SCANDIR_DIRNAME), AA_SCANDIR_DIRNAME "/");
                byte_copy (buf_dst + sizeof (AA_SCANDIR_DIRNAME), l_dst + 1, dst);

                if (symlink (buf_lnk, buf_dst) < 0)
                {
                    warn_fn (buf_dst, errno);
                    goto err;
                }
            }
        }
    }

    if (chmod (dst, mode) < 0)
    {
        if (has.began)
            warn_fn (dst, errno);
        goto err;
    }

    satmp.len = l_satmp;
    return 0;

err:
    satmp.len = l_satmp;
    if (!has.began)
        return -ERR_IO;
    else
    {
        unsigned int l_dst = strlen (dst);
        char buf[1 + l_dst + 1];

        *buf = '@';
        byte_copy (buf + 1, l_dst + 1, dst);

        /* rename dst servicedir by prefixing with a '@' so that aa-start would
         * fail to find/start the service, and make it easilly noticable on the
         * file system, since it's in an undetermined/invalid state */
        if (rename (dst, buf) < 0)
            warn_fn (dst, errno);

        return -ERR_FAILED_ENABLE;
    }
}

static int
copy_from_source (const char        *name,
                  int                len,
                  aa_warn_fn         warn_fn,
                  aa_enable_flags    flags,
                  aa_auto_enable_cb  ae_cb)
{
    int i;

    if (aa_sa_sources.len == 0)
        return -ERR_UNKNOWN;

    i = 0;
    for (;;)
    {
        int l_sce = strlen (aa_sa_sources.s + i);
        char buf[l_sce + 1 + len + 1];
        struct stat st;

        byte_copy (buf, l_sce, aa_sa_sources.s + i);
        buf[l_sce] = '/';
        byte_copy (buf + l_sce + 1, len, name);
        buf[l_sce + 1 + len] = '\0';

        if (stat (buf, &st) < 0)
        {
            if (errno != ENOENT)
                warn_fn (buf, errno);
        }
        else if (!S_ISDIR (st.st_mode))
            warn_fn (buf, ENOTDIR);
        else
        {
            int r;

            r = copy_dir (buf, name, st.st_mode, 0, warn_fn, flags, ae_cb,
                    (name[len - 1] == '@') ? name + len : NULL);
            if (r < 0)
                return r;
            break;
        }

        i += l_sce + 1;
        if (i > aa_sa_sources.len)
            return -ERR_UNKNOWN;
    }

    return 0;
}

int
aa_enable_service (const char       *_name,
                   aa_warn_fn        warn_fn,
                   aa_enable_flags   flags,
                   aa_auto_enable_cb ae_cb)
{
    const char *name = _name;
    const char *instance = NULL;
    mode_t _mode = 0; /* silence warning */
    int l_name = strlen (name);
    int len;
    int r;

    /* if name is a /path/to/file we get the actual/service name */
    if (*name == '/')
    {
        int r;

        if (l_name == 1)
            return -ERR_INVALID_NAME;
        r = byte_rchr (name, l_name, '/') + 1;
        name += r;
        l_name -= r;
    }

    if (!is_valid_service_name (name, l_name))
        return -ERR_INVALID_NAME;

    if (*_name == '/')
    {
        struct stat st;

        if (stat (_name, &st) < 0)
            return ERR_IO;
        else if (S_ISREG (st.st_mode))
            /* file; so nothing special to do, we can "drop" the path */
            _name = name;
        else if (!S_ISDIR (st.st_mode))
            return (errno = EINVAL, -ERR_IO);
        else
            _mode = st.st_mode;
    }

    /* len is l_name unless there's a '@', then we want up to (inc.) the '@' */
    len = byte_chr (name, l_name, '@');
    if (len < l_name)
    {
        ++len;
        instance = name + len;
    }

    r = copy_from_source (name, len, warn_fn, flags | _AA_FLAG_IS_SERVICEDIR, ae_cb);
    if (r < 0)
        return r;

    if (name != _name)
        return copy_dir (_name, name, _mode, 0, warn_fn, flags, ae_cb, instance);
    else
        return 0;
}
