/*	$OpenBSD: disksubr.c,v 1.15 2011/04/16 03:21:15 krw Exp $	*/
/*	$NetBSD: disksubr.c,v 1.21 1996/05/03 19:42:03 christos Exp $	*/

/*
 * Copyright (c) 1996 Theo de Raadt
 * Copyright (c) 1982, 1986, 1988 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/disklabel.h>
#include <sys/disk.h>

int	readdpmelabel(struct buf *, void (*)(struct buf *),
	    struct disklabel *, int *, int);

/*
 * Attempt to read a disk label from a device
 * using the indicated strategy routine.
 * The label must be partly set up before this:
 * secpercyl, secsize and anything required for a block i/o read
 * operation in the driver's strategy/start routines
 * must be filled in before calling us.
 *
 * If dos partition table requested, attempt to load it and
 * find disklabel inside a DOS partition.
 *
 * We would like to check if each MBR has a valid DOSMBR_SIGNATURE, but
 * we cannot because it doesn't always exist. So.. we assume the
 * MBR is valid.
 */
int
readdisklabel(dev_t dev, void (*strat)(struct buf *),
    struct disklabel *lp, int spoofonly)
{
	struct buf *bp = NULL;
	int error;

	if ((error = initdisklabel(lp)))
		goto done;

	/* get a buffer and initialize it */
	bp = geteblk((int)lp->d_secsize);
	bp->b_dev = dev;

	error = readdpmelabel(bp, strat, lp, NULL, spoofonly);
	if (error == 0)
		goto done;

	error = readdoslabel(bp, strat, lp, NULL, spoofonly);
	if (error == 0)
		goto done;

#if defined(CD9660)
	error = iso_disklabelspoof(dev, strat, lp);
	if (error == 0)
		goto done;
#endif
#if defined(UDF)
	error = udf_disklabelspoof(dev, strat, lp);
	if (error == 0)
		goto done;
#endif

done:
	if (bp) {
		bp->b_flags |= B_INVAL;
		brelse(bp);
	}
	disk_change = 1;
	return (error);
}

int
readdpmelabel(struct buf *bp, void (*strat)(struct buf *),
    struct disklabel *lp, int *partoffp, int spoofonly)
{
	int i, part_cnt, n, hfspartoff = -1;
	u_int64_t hfspartend = DL_GETDSIZE(lp);
	struct part_map_entry *part;

	/* First check for a DPME (HFS) disklabel */
	bp->b_blkno = 1;
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if (biowait(bp))
		return (bp->b_error);

	/* if successful, wander through DPME partition table */
	part = (struct part_map_entry *)bp->b_data;
	/* if first partition is not valid, assume not HFS/DPME partitioned */
	if (part->pmSig != PART_ENTRY_MAGIC)
		return (EINVAL);	/* not a DPME partition */
	part_cnt = part->pmMapBlkCnt;
	n = 8;
	for (i = 0; i < part_cnt; i++) {
		struct partition *pp;
		char *s;

		bp->b_blkno = 1+i;
		bp->b_bcount = lp->d_secsize;
		CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
		SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
		(*strat)(bp);
		if (biowait(bp))
			return (bp->b_error);

		part = (struct part_map_entry *)bp->b_data;
		/* toupper the string, in case caps are different... */
		for (s = part->pmPartType; *s; s++)
			if ((*s >= 'a') && (*s <= 'z'))
				*s = (*s - 'a' + 'A');

		if (strcmp(part->pmPartType, PART_TYPE_OPENBSD) == 0) {
			hfspartoff = part->pmPyPartStart - LABELSECTOR;
			hfspartend = hfspartoff + part->pmPartBlkCnt;
			if (partoffp) {
				*partoffp = hfspartoff;
				return (0);
			} else {
				DL_SETBSTART(lp, hfspartoff);
				DL_SETBEND(lp,
				    hfspartend < DL_GETDSIZE(lp) ? hfspartend :
				    DL_GETDSIZE(lp));
			}
			continue;
		}

		if (n >= MAXPARTITIONS || partoffp)
			continue;

		/* Currently we spoof HFS partitions only. */
		if (strcmp(part->pmPartType, PART_TYPE_MAC) == 0) {
			pp = &lp->d_partitions[n];
			DL_SETPOFFSET(pp, part->pmPyPartStart);
			DL_SETPSIZE(pp, part->pmPartBlkCnt);
			pp->p_fstype = FS_HFS;
			n++;
		}
	}

	if (hfspartoff == -1)
		return (EINVAL);

	if (spoofonly)
		return (0);

	/* next, dig out disk label */
	bp->b_blkno = hfspartoff + LABELSECTOR;
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if (biowait(bp))
		return (bp->b_error);

	return checkdisklabel(bp->b_data + LABELOFFSET, lp, hfspartoff,
	    hfspartend);
}

/*
 * Write disk label back to device after modification.
 */
int
writedisklabel(dev_t dev, void (*strat)(struct buf *), struct disklabel *lp)
{
	int error = EIO, partoff = -1;
	struct disklabel *dlp;
	struct buf *bp = NULL;

	/* get a buffer and initialize it */
	bp = geteblk((int)lp->d_secsize);
	bp->b_dev = dev;

	if (readdpmelabel(bp, strat, lp, &partoff, 1) != 0 &&
	    readdoslabel(bp, strat, lp, &partoff, 1) != 0)
		goto done;

	/* Read it in, slap the new label in, and write it back out */
	bp->b_blkno = partoff + LABELSECTOR;
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if ((error = biowait(bp)) != 0)
		goto done;

	dlp = (struct disklabel *)(bp->b_data + LABELOFFSET);
	*dlp = *lp;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_WRITE | B_RAW);
	(*strat)(bp);
	error = biowait(bp);

done:
	if (bp) {
		bp->b_flags |= B_INVAL;
		brelse(bp);
	}
	disk_change = 1;
	return (error);
}
