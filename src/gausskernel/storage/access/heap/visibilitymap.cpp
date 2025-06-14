/* -------------------------------------------------------------------------
 *
 * visibilitymap.cpp
 *	  bitmap for tracking visibility of heap tuples
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/heap/visibilitymap.cpp
 *
 * INTERFACE ROUTINES
 *		visibilitymap_clear  - clear a bit in the visibility map
 *		visibilitymap_pin	 - pin a map page for setting a bit
 *		visibilitymap_pin_ok - check whether correct map page is already pinned
 *		visibilitymap_set	 - set a bit in a previously pinned page
 *		visibilitymap_test	 - test if a bit is set
 *		visibilitymap_count  - count number of bits set in visibility map
 *		visibilitymap_truncate	- truncate the visibility map
 *
 * NOTES
 *
 * The visibility map is a bitmap with one bit per heap page. A set bit means
 * that all tuples on the page are known visible to all transactions, and
 * therefore the page doesn't need to be vacuumed. The map is conservative in
 * the sense that we make sure that whenever a bit is set, we know the
 * condition is true, but if a bit is not set, it might or might not be true.
 *
 * Clearing a visibility map bit is not separately WAL-logged.	The callers
 * must make sure that whenever a bit is cleared, the bit is cleared on WAL
 * replay of the updating operation as well.
 *
 * When we *set* a visibility map during VACUUM, we must write WAL.  This may
 * seem counterintuitive, since the bit is basically a hint: if it is clear,
 * it may still be the case that every tuple on the page is visible to all
 * transactions; we just don't know that for certain.  The difficulty is that
 * there are two bits which are typically set together: the PD_ALL_VISIBLE bit
 * on the page itself, and the visibility map bit.	If a crash occurs after the
 * visibility map page makes it to disk and before the updated heap page makes
 * it to disk, redo must set the bit on the heap page.	Otherwise, the next
 * insert, update, or delete on the heap page will fail to realize that the
 * visibility map bit must be cleared, possibly causing index-only scans to
 * return wrong answers.
 *
 * VACUUM will normally skip pages for which the visibility map bit is set;
 * such pages can't contain any dead tuples and therefore don't need vacuuming.
 * The visibility map is not used for freeze vacuums, because this needs to freeze
 * tuples and observe the latest xid present in the table, even on pages that don't
 * have any dead tuples.
 *
 * LOCKING
 *
 * In heapam.c, whenever a page is modified so that not all tuples on the
 * page are visible to everyone anymore, the corresponding bit in the
 * visibility map is cleared. In order to be crash-safe, we need to do this
 * while still holding a lock on the heap page and in the same critical
 * section that logs the page modification. However, we don't want to hold
 * the buffer lock over any I/O that may be required to read in the visibility
 * map page.  To avoid this, we examine the heap page before locking it;
 * if the page-level PD_ALL_VISIBLE bit is set, we pin the visibility map
 * bit.  Then, we lock the buffer.	But this creates a race condition: there
 * is a possibility that in the time it takes to lock the buffer, the
 * PD_ALL_VISIBLE bit gets set.  If that happens, we have to unlock the
 * buffer, pin the visibility map page, and relock the buffer.	This shouldn't
 * happen often, because only VACUUM currently sets visibility map bits,
 * and the race will only occur if VACUUM processes a given page at almost
 * exactly the same time that someone tries to further modify it.
 *
 * To set a bit, you need to hold a lock on the heap page. That prevents
 * the race condition where VACUUM sees that all tuples on the page are
 * visible to everyone, but another backend modifies the page before VACUUM
 * sets the bit in the visibility map.
 *
 * When a bit is set, the LSN of the visibility map page is updated to make
 * sure that the visibility map update doesn't get written to disk before the
 * WAL record of the changes that made it possible to set the bit is flushed.
 * But when a bit is cleared, we don't have to do that because it's always
 * safe to clear a bit in the map from correctness point of view.
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/heapam.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/buf/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr/smgr.h"
#include "utils/aiomem.h"
#include "utils/inval.h"
#include "commands/tablespace.h"
#include "catalog/pg_hashbucket_fn.h"
#include "catalog/pg_partition_fn.h"
#include "utils/syscache.h"
/* table for fast counting of set bits */
static const uint8 number_of_ones[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2,
    3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3,
    3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5,
    6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4,
    3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4,
    5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6,
    6, 7, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

/* prototypes for internal routines */
static Buffer vm_readbuf(Relation rel, BlockNumber blkno, bool extend);
static void vm_extend(Relation rel, BlockNumber nvmblocks);

/*
 *	visibilitymap_clear - clear a bit in visibility map
 *
 * You must pass a buffer containing the correct map page to this function.
 * Call visibilitymap_pin first to pin the right one. This function doesn't do
 * any I/O.
 */
void visibilitymap_clear(Relation rel, BlockNumber heapBlk, Buffer buf)
{
    BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

#ifdef TRACE_VISIBILITYMAP
    ereport(DEBUG1, (errmsg("vm_clear %s %d", RelationGetRelationName(rel), heapBlk)));
#endif

    if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != mapBlock)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("wrong buffer passed to visibilitymap_clear")));

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

    if (visibilitymap_clear_page(BufferGetPage(buf), heapBlk)) {
        MarkBufferDirty(buf);
    }

    LockBuffer(buf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_pin - pin a map page for setting a bit
 *
 * Setting a bit in the visibility map is a two-phase operation. First, call
 * visibilitymap_pin, to pin the visibility map page containing the bit for
 * the heap page. Because that can require I/O to read the map page, you
 * shouldn't hold a lock on the heap page while doing that. Then, call
 * visibilitymap_set to actually set the bit.
 *
 * On entry, *buf should be InvalidBuffer or a valid buffer returned by
 * an earlier call to visibilitymap_pin or visibilitymap_test on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the bit for heapBlk.
 *
 * If the page doesn't exist in the map file yet, it is extended.
 */
void visibilitymap_pin(Relation rel, BlockNumber heapBlk, Buffer *buf)
{
    BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

    /* Reuse the old pinned buffer if possible */
    if (BufferIsValid(*buf)) {
        if (BufferGetBlockNumber(*buf) == mapBlock)
            return;

        ReleaseBuffer(*buf);
    }
    *buf = vm_readbuf(rel, mapBlock, true);
}

/*
 * Do we already have the correct page pinned?
 * On entry, buf should be InvalidBuffer or a valid buffer returned by
 * an earlier call to visibilitymap_pin or visibilitymap_test on the same
 * relation.  The return value indicates whether the buffer covers the
 * given heapBlk.
 */
bool visibilitymap_pin_ok(BlockNumber heapBlk, Buffer buf)
{
    BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
    return BufferIsValid(buf) && (BufferGetBlockNumber(buf) == mapBlock);
}

/*
 *	visibilitymap_set - set a bit on a previously pinned page
 *
 * recptr is the LSN of the XLOG record we're replaying, if we're in recovery,
 * or InvalidXLogRecPtr in normal running.	The page LSN is advanced to the
 * one provided; in normal running, we generate a new XLOG record and set the
 * page LSN to that value.	cutoff_xid is the largest xmin on the page being
 * marked all-visible; it is needed for Hot Standby, and can be
 * InvalidTransactionId if the page contains no tuples.
 *
 * Caller is expected to set the heap page's PD_ALL_VISIBLE bit before calling
 * this function. Except in recovery, caller should also pass the heap
 * buffer. When page is logical and we're not in recovery, we must add
 * the heap buffer to the WAL chain.
 *
 * You must pass a buffer containing the correct map page to this function.
 * Call visibilitymap_pin first to pin the right one. This function doesn't do
 * any I/O.
 */
void visibilitymap_set(Relation rel, BlockNumber heapBlk, Buffer heapBuf, XLogRecPtr recptr, Buffer vmBuf,
                       TransactionId cutoff_xid, bool free_dict)
{
    BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
    Page page;

#ifdef TRACE_VISIBILITYMAP
    ereport(DEBUG1, (errmsg("vm_set %s %d", RelationGetRelationName(rel), heapBlk)));
#endif

    Assert(t_thrd.xlog_cxt.InRecovery || XLogRecPtrIsInvalid(recptr));

    /* Check that we have the right VM page pinned */
    if (!BufferIsValid(vmBuf) || BufferGetBlockNumber(vmBuf) != mapBlock)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("wrong VM buffer passed to visibilitymap_set")));

    page = BufferGetPage(vmBuf);

    LockBuffer(vmBuf, BUFFER_LOCK_EXCLUSIVE);

    START_CRIT_SECTION();
    if (visibilitymap_set_page(page, heapBlk)) {
        MarkBufferDirty(vmBuf);

        if (RelationNeedsWAL(rel)) {
            if (XLogRecPtrIsInvalid(recptr)) {
                Assert(!t_thrd.xlog_cxt.InRecovery);
                recptr = log_heap_visible(rel->rd_node, heapBlk, heapBuf, vmBuf, cutoff_xid, free_dict);

                /*
                 * If data checksums are enabled (or wal_log_hints=on), we
                 * need to protect the heap page from being torn.
                 */
                if (XLogHintBitIsNeeded() && BufferIsValid(heapBuf)) {
                    Page heapPage = BufferGetPage(heapBuf);

                    /* caller is expected to set PD_ALL_VISIBLE first */
                    Assert(PageIsAllVisible(heapPage));
                    if (ENABLE_DMS) {
                        BufferDesc* buf_desc = GetBufferDescriptor(heapBuf - 1);
                        if ((pg_atomic_read_u64(&buf_desc->state) & BM_DIRTY) == 0) {
                            MarkBufferDirty(heapBuf);
                        }
                    }

                    PageSetLSN(heapPage, recptr);
                }
            }
            PageSetLSN(page, recptr);
        }
    }
    END_CRIT_SECTION();

    LockBuffer(vmBuf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_test - test if a bit is set
 *
 * Are all tuples on heapBlk visible to all, according to the visibility map?
 *
 * On entry, *buf should be InvalidBuffer or a valid buffer returned by an
 * earlier call to visibilitymap_pin or visibilitymap_test on the same
 * relation. On return, *buf is a valid buffer with the map page containing
 * the bit for heapBlk, or InvalidBuffer. The caller is responsible for
 * releasing *buf after it's done testing and setting bits.
 *
 * NOTE: This function is typically called without a lock on the heap page,
 * so somebody else could change the bit just after we look at it.	In fact,
 * since we don't lock the visibility map page either, it's even possible that
 * someone else could have changed the bit just before we look at it, but yet
 * we might see the old value.	It is the caller's responsibility to deal with
 * all concurrency issues!
 */
bool visibilitymap_test(Relation rel, BlockNumber heapBlk, Buffer *buf)
{
    if (ENABLE_DMS && !SS_PRIMARY_MODE) {
        return false;
    }

    BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
    uint32 mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
    uint8 mapBit = HEAPBLK_TO_MAPBIT(heapBlk);
    bool result = false;
    unsigned char *map = NULL;

#ifdef TRACE_VISIBILITYMAP
    ereport(DEBUG1, (errmsg("vm_test %s %d", RelationGetRelationName(rel), heapBlk)));
#endif

    /* Reuse the old pinned buffer if possible */
    if (BufferIsValid(*buf)) {
        volatile BufferDesc *bufHdr = NULL;

        if (BufferIsLocal(*buf)) {
            bufHdr = (BufferDesc *)&(u_sess->storage_cxt.LocalBufferDescriptors[-(*buf) - 1].bufferdesc);
        } else {
            bufHdr = GetBufferDescriptor(*buf - 1);
        }

        if (bufHdr->tag.blockNum != mapBlock || bufHdr->tag.rnode.bucketNode != rel->rd_node.bucketNode) {
            ReleaseBuffer(*buf);
            *buf = InvalidBuffer;
        }
    }

    if (!BufferIsValid(*buf)) {
        *buf = vm_readbuf(rel, mapBlock, false);
        if (!BufferIsValid(*buf))
            return false;
    }

    map = (unsigned char *)PageGetContents(BufferGetPage(*buf));

    /*
     * A single-bit read is atomic.  There could be memory-ordering effects
     * here, but for performance reasons we make it the caller's job to worry
     * about that.
     */
    result = (map[mapByte] & ((uint32)1 << mapBit)) ? true : false;

    return result;
}

BlockNumber visibilitymap_count_heap(Relation rel)
{
    BlockNumber result = 0;
    BlockNumber mapBlock;

    for (mapBlock = 0;; mapBlock++) {
        Buffer mapBuffer;
        unsigned char *map = NULL;
        unsigned int i;

        /*
         * Read till we fall off the end of the map.  We assume that any extra
         * bytes in the last page are zeroed, so we don't bother excluding
         * them from the count.
         */
        mapBuffer = vm_readbuf(rel, mapBlock, false);
        if (!BufferIsValid(mapBuffer))
            break;

        /*
         * We choose not to lock the page, since the result is going to be
         * immediately stale anyway if anyone is concurrently setting or
         * clearing bits, and we only really need an approximate value.
         */
        map = (unsigned char *)PageGetContents(BufferGetPage(mapBuffer));

        for (i = 0; i < MAPSIZE; i++) {
            result += number_of_ones[map[i]];
        }

        ReleaseBuffer(mapBuffer);
    }
    return result;
}

/*
 *	visibilitymap_count  - count number of bits set in visibility map
 *
 * Note: we ignore the possibility of race conditions when the table is being
 * extended concurrently with the call.  New pages added to the table aren't
 * going to be marked all-visible, so they won't affect the result.
 */
BlockNumber visibilitymap_count(Relation rel, Partition part)
{
    Relation buckRel = NULL;
    BlockNumber result = 0;

    if (RELATION_OWN_BUCKET(rel)) {
        oidvector *bucketlist = searchHashBucketByOid(rel->rd_bucketoid);

        for (int i = 0; i < bucketlist->dim1; i++) {
            buckRel = bucketGetRelation(rel, part, bucketlist->values[i]);
            result += visibilitymap_count_heap(buckRel);
            bucketCloseRelation(buckRel);
        }
    } else if (RelationIsPartitioned(rel) && PointerIsValid(part)) {
        Relation partRel = partitionGetRelation(rel, part);
        if (RelationIsSubPartitioned(rel) && PartitionIsTablePartition(part)) {
            List *subPartList = relationGetPartitionList(partRel, NoLock);
            ListCell *lc = NULL;
            foreach (lc, subPartList) {
                Partition subPart = (Partition)lfirst(lc);
                Relation subPartRel = partitionGetRelation(rel, subPart);
                result += visibilitymap_count_heap(subPartRel);
                releaseDummyRelation(&subPartRel);
            }
            releasePartitionList(partRel, &subPartList, NoLock);
        } else {
            result = visibilitymap_count_heap(partRel);
        }
        releaseDummyRelation(&partRel);
    } else {
        result = visibilitymap_count_heap(rel);
    }

    return result;
}

void XLogBlockTruncateRelVM(Relation rel, BlockNumber nheapblocks)
{
    BlockNumber newnblocks;
    /* last remaining block, byte, and bit */
    BlockNumber truncBlock = HEAPBLK_TO_MAPBLOCK(nheapblocks);
    uint32 truncByte = HEAPBLK_TO_MAPBYTE(nheapblocks);
    uint8 truncBit = HEAPBLK_TO_MAPBIT(nheapblocks);

#ifdef TRACE_VISIBILITYMAP
    ereport(DEBUG1, (errmsg("xlog_block_vm_truncate %s %d", RelationGetRelationName(rel), nheapblocks)));
#endif

    RelationOpenSmgr(rel);

    /*
     * If no visibility map has been created yet for this relation, there's
     * nothing to truncate.
     */
    if (!smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
        return;

    /*
     * Unless the new size is exactly at a visibility map page boundary, the
     * tail bits in the last remaining map page, representing truncated heap
     * blocks, need to be cleared. This is not only tidy, but also necessary
     * because we don't get a chance to clear the bits if the heap is extended
     * again.
     */
    if (truncByte != 0 || truncBit != 0) {
        newnblocks = truncBlock + 1;

    } else
        newnblocks = truncBlock;

    if (smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM) <= newnblocks) {
        /* nothing to do, the file was already smaller than requested size */
        return;
    }

    /* Truncate the unused VM pages, and send smgr inval message */
    smgrtruncatefunc(rel->rd_smgr, VISIBILITYMAP_FORKNUM, newnblocks);
    XLogTruncateRelation(rel->rd_node, VISIBILITYMAP_FORKNUM, newnblocks);
    /*
     * We might as well update the local smgr_vm_nblocks setting. smgrtruncate
     * sent an smgr cache inval message, which will cause other backends to
     * invalidate their copy of smgr_vm_nblocks, and this one too at the next
     * command boundary.  But this ensures it isn't outright wrong until then.
     */
    if (rel->rd_smgr)
        rel->rd_smgr->smgr_vm_nblocks = newnblocks;
}

/*
 *	visibilitymap_truncate - truncate the visibility map
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access the VM again.
 *
 * nheapblocks is the new size of the heap.
 */
void visibilitymap_truncate(Relation rel, BlockNumber nheapblocks)
{
    errno_t rc = EOK;
    BlockNumber newnblocks;

    /* last remaining block, byte, and bit */
    BlockNumber truncBlock = HEAPBLK_TO_MAPBLOCK(nheapblocks);
    uint32 truncByte = HEAPBLK_TO_MAPBYTE(nheapblocks);
    uint8 truncBit = HEAPBLK_TO_MAPBIT(nheapblocks);

#ifdef TRACE_VISIBILITYMAP
    ereport(DEBUG1, (errmsg("vm_truncate %s %d", RelationGetRelationName(rel), nheapblocks)));
#endif

    RelationOpenSmgr(rel);

    /*
     * If no visibility map has been created yet for this relation, there's
     * nothing to truncate.
     */
    if (!smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
        return;

    /*
     * Unless the new size is exactly at a visibility map page boundary, the
     * tail bits in the last remaining map page, representing truncated heap
     * blocks, need to be cleared. This is not only tidy, but also necessary
     * because we don't get a chance to clear the bits if the heap is extended
     * again.
     */
    if (truncByte != 0 || truncBit != 0) {
        Buffer mapBuffer;
        Page page;
        unsigned char *map = NULL;

        newnblocks = truncBlock + 1;

        mapBuffer = vm_readbuf(rel, truncBlock, false);
        if (!BufferIsValid(mapBuffer)) {
            /* nothing to do, the file was already smaller */
            return;
        }

        page = BufferGetPage(mapBuffer);
        map = (unsigned char *)PageGetContents(page);

        LockBuffer(mapBuffer, BUFFER_LOCK_EXCLUSIVE);

        /* NO EREPORT(ERROR) from here till changes are logged */
        START_CRIT_SECTION();

        /* Clear out the unwanted bytes. */
        rc = memset_s(&map[truncByte + 1], MAPSIZE - (truncByte + 1), 0, MAPSIZE - (truncByte + 1));
        securec_check(rc, "\0", "\0");

        /*
         * Mask out the unwanted bits of the last remaining byte.
         *
         * ((1 << 0) - 1) = 00000000 ((1 << 1) - 1) = 00000001 ... ((1 << 6) -
         * 1) = 00111111 ((1 << 7) - 1) = 01111111
         */
        map[truncByte] &= ((uint32)1 << truncBit) - 1;

        /*
         * Truncation of a relation is WAL-logged at a higher-level, and we
         * will be called at WAL replay. But if checksums are enabled, we need
         * to still write a WAL record to protect against a torn page, if the
         * page is flushed to disk before the truncation WAL record. We cannot
         * use MarkBufferDirtyHint here, because that will not dirty the page
         * during recovery.
         */
        MarkBufferDirty(mapBuffer);
        if (!t_thrd.xlog_cxt.InRecovery && RelationNeedsWAL(rel) && XLogHintBitIsNeeded()) {
            log_newpage_buffer(mapBuffer, false);
        }

        END_CRIT_SECTION();

        UnlockReleaseBuffer(mapBuffer);
    } else
        newnblocks = truncBlock;

    if (smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM) <= newnblocks) {
        /* nothing to do, the file was already smaller than requested size */
        return;
    }

    /* Truncate the unused VM pages, and send smgr inval message */
    smgrtruncate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, newnblocks);

    /*
     * We might as well update the local smgr_vm_nblocks setting. smgrtruncate
     * sent an smgr cache inval message, which will cause other backends to
     * invalidate their copy of smgr_vm_nblocks, and this one too at the next
     * command boundary.  But this ensures it isn't outright wrong until then.
     */
    if (rel->rd_smgr)
        rel->rd_smgr->smgr_vm_nblocks = newnblocks;
}

/*
 * Read a visibility map page.
 *
 * If the page doesn't exist, InvalidBuffer is returned, or if 'extend' is
 * true, the visibility map file is extended.
 */
static Buffer vm_readbuf(Relation rel, BlockNumber blkno, bool extend)
{
    Buffer buf;

    /*
     * We might not have opened the relation at the smgr level yet, or we
     * might have been forced to close it by a sinval message.	The code below
     * won't necessarily notice relation extension immediately when extend =
     * false, so we rely on sinval messages to ensure that our ideas about the
     * size of the map aren't too far out of date.
     */
    RelationOpenSmgr(rel);

    /*
     * If we haven't cached the size of the visibility map fork yet, check it
     * first.
     */
    if (rel->rd_smgr->smgr_vm_nblocks == InvalidBlockNumber) {
        if (smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
            rel->rd_smgr->smgr_vm_nblocks = smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM);
        else
            rel->rd_smgr->smgr_vm_nblocks = 0;
    }

    /* Handle requests beyond EOF */
    if (blkno >= rel->rd_smgr->smgr_vm_nblocks) {
        if (extend)
            vm_extend(rel, blkno + 1);
        else
            return InvalidBuffer;
    }

    /*
     * Use ZERO_ON_ERROR mode, and initialize the page if necessary. It's
     * always safe to clear bits, so it's better to clear corrupt pages than
     * error out.
     */
    buf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, blkno, RBM_ZERO_ON_ERROR, NULL);
    if (PageIsNew(BufferGetPage(buf))) {
        PageInit(BufferGetPage(buf), BLCKSZ, 0);
    }

    return buf;
}

/*
 * Ensure that the visibility map fork is at least vm_nblocks long, extending
 * it if necessary with zeroed pages.
 */
static void vm_extend(Relation rel, BlockNumber vm_nblocks)
{
    BlockNumber vm_nblocks_now;
    Page pg;
    Page pg_ori = NULL;

    ADIO_RUN()
    {
        pg = (Page)adio_align_alloc(BLCKSZ);
    }
    ADIO_ELSE()
    {
        if (ENABLE_DSS) {
            pg_ori = (Page)palloc(BLCKSZ + ALIGNOF_BUFFER);
            pg = (Page)BUFFERALIGN(pg_ori);
        } else {
            pg = (Page)palloc(BLCKSZ);
        }
    }
    ADIO_END();

    PageInit(pg, BLCKSZ, 0);

    /*
     * We use the relation extension lock to lock out other backends trying to
     * extend the visibility map at the same time. It also locks out extension
     * of the main fork, unnecessarily, but extending the visibility map
     * happens seldom enough that it doesn't seem worthwhile to have a
     * separate lock tag type for it.
     *
     * Note that another backend might have extended or created the relation
     * by the time we get the lock.
     */
    LockRelationForExtension(rel, ExclusiveLock);

    /* Might have to re-open if a cache flush happened */
    RelationOpenSmgr(rel);

    /*
     * Create the file first if it doesn't exist.  If smgr_vm_nblocks is
     * positive then it must exist, no need for an smgrexists call.
     */
    if ((rel->rd_smgr->smgr_vm_nblocks == 0 || rel->rd_smgr->smgr_vm_nblocks == InvalidBlockNumber) &&
        !smgrexists(rel->rd_smgr, VISIBILITYMAP_FORKNUM))
        smgrcreate(rel->rd_smgr, VISIBILITYMAP_FORKNUM, t_thrd.xlog_cxt.InRecovery);

    vm_nblocks_now = smgrnblocks(rel->rd_smgr, VISIBILITYMAP_FORKNUM);
    // check tablespace size limitation when extending VM file.
    if (vm_nblocks_now < vm_nblocks) {
        STORAGE_SPACE_OPERATION(rel, ((uint64)BLCKSZ) * (vm_nblocks - vm_nblocks_now));
        RelationOpenSmgr(rel);
    }

    /* Now extend the file */
    while (vm_nblocks_now < vm_nblocks) {
        if (IsSegmentFileNode(rel->rd_node)) {
            Buffer buf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, P_NEW, RBM_ZERO, NULL);
            ReleaseBuffer(buf);
#ifdef USE_ASSERT_CHECKING
            BufferDesc *buf_desc = GetBufferDescriptor(buf - 1);
            Assert(buf_desc->tag.blockNum == vm_nblocks_now);
#endif
        } else {
            PageSetChecksumInplace(pg, vm_nblocks_now);
            smgrextend(rel->rd_smgr, VISIBILITYMAP_FORKNUM, vm_nblocks_now, (char *)pg, false);
        }
        vm_nblocks_now++;
    }

    /*
     * Send a shared-inval message to force other backends to close any smgr
     * references they may have for this rel, which we are about to change.
     * This is a useful optimization because it means that backends don't have
     * to keep checking for creation or extension of the file, which happens
     * infrequently.
     */
    CacheInvalidateSmgr(rel->rd_smgr->smgr_rnode);

    /* Update local cache with the up-to-date size */
    rel->rd_smgr->smgr_vm_nblocks = vm_nblocks_now;

    UnlockRelationForExtension(rel, ExclusiveLock);

    ADIO_RUN()
    {
        adio_align_free(pg);
        pg = NULL;
    }
    ADIO_ELSE()
    {
        if (ENABLE_DSS) {
            pfree(pg_ori);
            pg_ori = NULL;
        } else {
            pfree(pg);
            pg = NULL;
        }
    }
    ADIO_END();
}

BlockNumber VisibilityMapCalTruncBlkNo(BlockNumber relBlkNo)
{
    BlockNumber newnblocks;

    newnblocks = HEAPBLK_TO_MAPBLOCK(relBlkNo);

    return newnblocks;
}
