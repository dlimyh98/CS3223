/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include <stdio.h>
#include <stdlib.h>

#include <assert.h>

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))

/*********************************************/
// CS3223 - Data Structure declarations
typedef struct node {
	struct node* prev;
	struct node* next;
	int frame_id;
} node;

typedef struct info {
	struct node* head;
	struct node* tail;
	int size;
	slock_t linkedListInfo_spinlock;
} info;

static node* doubleLinkedList = NULL; //Make Global Declare it in InitializeStructure
static info* linkedListInfo = NULL;
node* search_for_frame(int desired_frame_id);
void delete_arbitrarily(int frame_id_for_deletion);
void insert_at_head(node* frame);
void move_to_head(node* frame);       // Case 1 - Called by StrategyAccessBuffer(..., false) in bufmgr_lru.c

/*********************************************/
// CS3223 - Function definitions

// Traverse through linkedListInfo for frame corresponding to some 'frame_id'
node* search_for_frame(int desired_frame_id) {
	node* traversal_ptr;

	if (linkedListInfo->head == NULL) { 
		//elog(ERROR, "linkedListInfo is empty");
		return NULL; // Handle empty list case properly
	} else {
		traversal_ptr = linkedListInfo->head;

		while (traversal_ptr != NULL) {
			if (traversal_ptr->frame_id == desired_frame_id) {
				return traversal_ptr;
			}

			traversal_ptr = traversal_ptr->next;
		}
	}

	return NULL; // Return NULL if frame_id not found
}

void delete_arbitrarily(int frame_id_for_deletion) {
	node* frame_for_deletion = search_for_frame(frame_id_for_deletion);

	if (!frame_for_deletion) { // Handle case where frame is not found
		return;
	}

	if (frame_for_deletion == linkedListInfo->head) { // Correctly check and update head
		if (linkedListInfo->head->next) { // Check if there's a next node
			linkedListInfo->head = linkedListInfo->head->next;
			linkedListInfo->head->prev = NULL;
		} else { // List becomes empty
			linkedListInfo->head = linkedListInfo->tail = NULL;
		}
		} else if (frame_for_deletion == linkedListInfo->tail) { // Correctly check and update tail
			linkedListInfo->tail = linkedListInfo->tail->prev;
			linkedListInfo->tail->next = NULL;
		} else { // Node is in the middle
			frame_for_deletion->prev->next = frame_for_deletion->next;
			frame_for_deletion->next->prev = frame_for_deletion->prev;
	}
}

void insert_at_head(node* frame) { 
	frame->next = linkedListInfo->head; 
	if (linkedListInfo->head != NULL) { // Check if list is not empty
		linkedListInfo->head->prev = frame; 
	}
	linkedListInfo->head = frame; 

	if (linkedListInfo->tail == NULL) { // If list was empty, update tail as well
		linkedListInfo->tail = frame;
	}

	frame->prev = NULL; // Set frame's prev to NULL
} 

void move_to_head(node* frame) { 
	delete_arbitrarily(frame->frame_id);
	insert_at_head(frame); 
}

char* print_list_to_string(info* linkedListInfo) {
    // Initial allocation for the string
    int str_size = 256; // Initial size, may need to increase depending on list size
    char* list_str = malloc(str_size * sizeof(char));
    if (!list_str) {
        // Handle memory allocation failure
        return NULL;
    }

    list_str[0] = '\0'; // Start with an empty string

    node* current = linkedListInfo->head;
    int offset = 0; // Keep track of the number of characters written

    while (current != NULL) {
        // Check remaining buffer size and reallocate if necessary
        if (str_size - offset < 50) { // Ensure there's at least 50 chars of space
            str_size *= 2; // Double the buffer size
            list_str = realloc(list_str, str_size);
            if (!list_str) {
                // Handle memory allocation failure
                return NULL;
            }
        }

        // Append current node's frame_id to the string
        int written = snprintf(list_str + offset, str_size - offset, "Frame ID: %d -> ", current->frame_id);
        if (written > 0) {
            offset += written; // Increase offset by the number of characters written
        } else {
            // Handle snprintf error
            free(list_str);
            return NULL;
        }

        current = current->next;
    }

    // Optionally, remove the last arrow " -> " for aesthetics
    if (offset > 4) {
        list_str[offset - 4] = '\0';
    }

    return list_str; // Caller is responsible for freeing the memory
}

void log_linked_list(info* linkedListInfo) {
    char* list_representation = print_list_to_string(linkedListInfo);
    if (list_representation) {
        elog(NOTICE, "LinkedList: %s", list_representation);
        free(list_representation);
    } else {
        elog(ERROR, "Failed to allocate memory for list representation");
    }
}
/*********************************************/



/*
 * The shared freelist control information.
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t		buffer_strategy_lock;

	/*
	 * Clock sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int			firstFreeBuffer;	/* Head of list of unused buffers */
	int			lastFreeBuffer; /* Tail of list of unused buffers */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock sweep */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


void StrategyAccessBuffer(int buf_id, bool delete); /* cs3223 */

/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer -- a lockless check to see if there is a free buffer in
 *					   buffer pool.
 *
 * If the result is true that will become stale once free buffers are moved out
 * by other operations, so the caller who strictly want to use a free buffer
 * should not call this.
 */
bool
have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}


// cs3223
// StrategyAccessBuffer 
// Called by bufmgr when a buffer page is accessed.
// Adjusts the position of buffer (identified by buf_id) in the LRU stack if delete is false;
// otherwise, delete buffer buf_id from the LRU stack.
void
StrategyAccessBuffer(int buf_id, bool delete)
{
	node* frame;
	if (delete) {
        SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "SpinLOCK A");
		//log_linked_list(linkedListInfo);

        delete_arbitrarily(buf_id);

        SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "SpinRELEASE A");
		//log_linked_list(linkedListInfo);
    } else {
		SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "SpinLOCK B");
		//log_linked_list(linkedListInfo);
		frame = search_for_frame(buf_id);

		if (frame) {
			move_to_head(frame);
		} else {
			node* new_frame = &doubleLinkedList[buf_id];
			new_frame->frame_id = buf_id;
			insert_at_head(new_frame);
		}

		SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "SpinRELEASE B");
		//log_linked_list(linkedListInfo);
	}
}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */

	// CS3223
	node* traversal_frame;
	int fetched_frame_id;
	node *fetched_frame;

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */

	// CS3223 - We check the freelist first before doing anything (strictly following LRU policy)
	// if (strategy != NULL)
	// {
	// 	buf = GetBufferFromRing(strategy, buf_state);
	// 	if (buf != NULL)
	// 	{
	// 		*from_ring = true;
	// 		StrategyAccessBuffer(buf->buf_id, false); // cs3223
	// 		return buf;
	// 	}
	// }

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * First check, without acquiring the lock, whether there's buffers in the
	 * freelist. Since we otherwise don't require the spinlock in every
	 * StrategyGetBuffer() invocation, it'd be sad to acquire it here -
	 * uselessly in most cases. That obviously leaves a race where a buffer is
	 * put on the freelist but we don't see the store yet - but that's pretty
	 * harmless, it'll just get used during the next buffer acquisition.
	 *
	 * If there's buffers on the freelist, acquire the spinlock to pop one
	 * buffer of the freelist. Then check whether that buffer is usable and
	 * repeat if not.
	 *
	 * Note that the freeNext fields are considered to be protected by the
	 * buffer_strategy_lock not the individual buffer spinlocks, so it's OK to
	 * manipulate them without holding the spinlock.
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* Acquire the spinlock to remove element from the freelist */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{


				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

			/* Unconditionally remove buffer from freelist */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * Release the lock so someone else can access the freelist while
			 * we check out this buffer.
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; discard it and retry.  (This can only happen if VACUUM
			 * put a valid buffer in the freelist and then someone else used
			 * it before we got to it.  It's probably impossible altogether as
			 * of 8.3, but we'd better check anyway.)
			 */
			local_buf_state = LockBufHdr(buf);
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
				&& BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			{
				
				// SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);
				//elog(LOG, "Case 2");
				// //log_linked_list(linkedListInfo);
			
				// AddBufferToRing(strategy, buf);
				//CS3223: Add buffer to the head of the linked list
				StrategyAccessBuffer(buf->buf_id, false);                      // Case 2
				// SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
				//elog(LOG, "Case 2");
				////log_linked_list(linkedListInfo);
				*buf_state = local_buf_state;
				return buf;
			}
			UnlockBufHdr(buf, local_buf_state);
		}
	}


	/**************** Nothing on the freelist, so we run the LRU algorithm below ... ****************/
	// 1. Start from tail
	// 2. Traverse to head, while checking for a suitable frame to evict

	SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);    // Acquire DLL lock
	//elog(LOG, "SpinLOCK Case 3");
	//log_linked_list(linkedListInfo);
	traversal_frame = linkedListInfo->tail;				          // Reset traversal to the tail
	trycounter = NBuffers;                                        // NOTE: NBuffers is 16 (as defined by Chee Yong) 

	// Case 3
	for (;;)
	{

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; decrement the usage_count (unless pinned) and keep scanning.
		 */

		if (traversal_frame == NULL) {
			// We must have traversed the entire list, or the list is empty
			// i.e All buffers are pinned

			// Thus, the result should be similar to Clock Policy (where all frames are pinned, none can be evicted)
			// We follow their method there
			traversal_frame = linkedListInfo->tail;				          // Reset traversal to the tail
			SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);    // Release the DLL spinlock we acquired before for(;;)
			elog(ERROR, "no unpinned buffers available");                 // Throw an error (and exit)
		}

		fetched_frame_id = traversal_frame->frame_id;
		buf = GetBufferDescriptor(fetched_frame_id);
		local_buf_state = LockBufHdr(buf);

		//elog(NOTICE, "fetched_frame is %d", fetched_frame_id);
		//elog(NOTICE, "RC is %d", BUF_STATE_GET_REFCOUNT(local_buf_state));

		// Check if the frame_id will be valid below...
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			//elog(LOG, "Entered: if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0) ");
			// if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			// {
			// 	local_buf_state -= BUF_USAGECOUNT_ONE;
			// 	trycounter = NBuffers;
			// }
			// else
			//{
				/* Found a usable buffer */
				if (strategy != NULL) {
					//elog(LOG, "Non-default strategy found a buffer");
				}
				// AddBufferToRing(strategy, buf);

			fetched_frame = search_for_frame(fetched_frame_id);
			move_to_head(fetched_frame);
			SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
			//elog(LOG, "SpinRELEASE Case 3 else");
			//log_linked_list(linkedListInfo);
			*buf_state = local_buf_state;
			return buf;
			//}
		}
		// else if (--trycounter == 0)
		// {
		// 	/*
		// 	 * We've scanned all the buffers without making any state changes,
		// 	 * so all the buffers are pinned (or were when we looked at them).
		// 	 * We could hope that someone will free one eventually, but it's
		// 	 * probably better to fail than to risk getting stuck in an
		// 	 * infinite loop.
		// 	 */
		// 	UnlockBufHdr(buf, local_buf_state);
		// 	SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
		// 	elog(LOG, "SpinRELEASE Case 3 elseif");
		// 	//log_linked_list(linkedListInfo);
		// 	elog(ERROR, "no unpinned buffers available");
		// }
		UnlockBufHdr(buf, local_buf_state);
		traversal_frame = traversal_frame -> prev;
	}
}

/*
 * StrategyFreeBuffer: put a buffer on the freelist
 */
void
StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	//elog(LOG, "SpinLOCK Case 4");
	//log_linked_list(linkedListInfo);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;


		// Case 4
		StrategyAccessBuffer(buf->buf_id, true);
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	//elog(LOG, "SpinRELEASE Case 4");
	//log_linked_list(linkedListInfo);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	// CS3223: Allocate size for our data structures in FREE-LIST
	size = add_size(size, sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS));

	//Size of the control information of double link list
	size = add_size(size, sizeof(info));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	// CS3223: Boolean values for if shared memory alloc is successful
	bool is_dll_success = false;
	bool is_link_list_info_success = false;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);


	// CS3223: Initialize space for our data structures
	// Linked List Info
	linkedListInfo = (info *)ShmemInitStruct("Link List Info",
												sizeof(info),
												&is_link_list_info_success);

	// Double Linked List itself
	doubleLinkedList = (node *)ShmemInitStruct("Double Link List",
														sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS),
														&is_dll_success);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Initialize the clock sweep pointer */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* Clear statistics */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* No pending notification */
		StrategyControl->bgwprocno = -1;
	}
	else
		Assert(!init);

	// CS3223: Intialize our DLL Data Structure
	if (!is_dll_success && !is_link_list_info_success) { //Initiate our Double Link List Data Structure here
		Assert (init);
		SpinLockInit(&linkedListInfo->linkedListInfo_spinlock);

		linkedListInfo->tail = NULL;
		linkedListInfo->size = 0;
		linkedListInfo->head = doubleLinkedList;
	} else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			ring_size_kb = 256;
			break;
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 256;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The bufhdr spin lock is held on the returned buffer.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * If the buffer is pinned we cannot use it under any circumstances.
	 *
	 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
	 * since our own previous usage of the ring element would have left it
	 * there, but it might've been decremented by clock sweep since then). A
	 * higher usage_count indicates someone else has touched the buffer, so we
	 * shouldn't re-use it.
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
		&& BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}
