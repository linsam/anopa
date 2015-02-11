
#ifndef AA_ENABLE_SERVICE_H
#define AA_ENABLE_SERVICE_H

#include <skalibs/stralloc.h>
#include <anopa/common.h>

typedef enum
{
    AA_FLAG_AUTO_ENABLE_NEEDS   = (1 << 0),
    AA_FLAG_AUTO_ENABLE_WANTS   = (1 << 1),
    /* private */
    _AA_FLAG_IS_SERVICEDIR      = (1 << 2),
    _AA_FLAG_IS_NEEDS           = (1 << 3),
    _AA_FLAG_IS_WANTS           = (1 << 4),
    _AA_FLAG_IS_BEF_AFT         = (1 << 5),
    _AA_FLAG_IS_1OF4            = _AA_FLAG_IS_NEEDS | _AA_FLAG_IS_WANTS | _AA_FLAG_IS_BEF_AFT
} aa_enable_flags;

extern stralloc aa_sa_sources;

typedef void (*aa_warn_fn) (const char *name, int err);
typedef void (*aa_auto_enable_cb) (const char *name, aa_enable_flags type);

extern int aa_enable_service (const char        *name,
                              aa_warn_fn         warn_fn,
                              aa_enable_flags    flags,
                              aa_auto_enable_cb  ae_cb);

#endif /* AA_ENABLE_SERVICE_H */
