/*
 * linux/fs/nfsd/auth.c
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcauth.h>
#include <linux/nfsd/nfsd.h>
#include <linux/vserver/xid.h>

#define	CAP_NFSD_MASK (CAP_FS_MASK|CAP_TO_MASK(CAP_SYS_RESOURCE))

int nfsd_setuser(struct svc_rqst *rqstp, struct svc_export *exp)
{
	struct svc_cred	cred = rqstp->rq_cred;
	int i;
	int ret;

	if (exp->ex_flags & NFSEXP_ALLSQUASH) {
		cred.cr_uid = exp->ex_anon_uid;
		cred.cr_gid = exp->ex_anon_gid;
		cred.cr_group_info = groups_alloc(0);
	} else if (exp->ex_flags & NFSEXP_ROOTSQUASH) {
		struct group_info *gi;
		if (!cred.cr_uid)
			cred.cr_uid = exp->ex_anon_uid;
		if (!cred.cr_gid)
			cred.cr_gid = exp->ex_anon_gid;
		gi = groups_alloc(cred.cr_group_info->ngroups);
		if (gi)
			for (i = 0; i < cred.cr_group_info->ngroups; i++) {
				if (!GROUP_AT(cred.cr_group_info, i))
					GROUP_AT(gi, i) = exp->ex_anon_gid;
				else
					GROUP_AT(gi, i) = GROUP_AT(cred.cr_group_info, i);
			}
		cred.cr_group_info = gi;
	} else
		get_group_info(cred.cr_group_info);

	if (cred.cr_uid != (uid_t) -1)
		current->fsuid = INOXID_UID(XID_TAG_NFSD, cred.cr_uid, cred.cr_gid);
	else
		current->fsuid = exp->ex_anon_uid;
	if (cred.cr_gid != (gid_t) -1)
		current->fsgid = INOXID_GID(XID_TAG_NFSD, cred.cr_uid, cred.cr_gid);
	else
		current->fsgid = exp->ex_anon_gid;

	/* this desperately needs a tag :) */
	current->xid = INOXID_XID(XID_TAG_NFSD, cred.cr_uid, cred.cr_gid, 0);

	if (!cred.cr_group_info)
		return -ENOMEM;
	ret = set_current_groups(cred.cr_group_info);
	put_group_info(cred.cr_group_info);
	if (INOXID_UID(XID_TAG_NFSD, cred.cr_uid, cred.cr_gid)) {
		cap_t(current->cap_effective) &= ~CAP_NFSD_MASK;
	} else {
		cap_t(current->cap_effective) |= (CAP_NFSD_MASK &
						  current->cap_permitted);
	}
	return ret;
}
