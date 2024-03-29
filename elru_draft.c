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
#define SECOND_LAST_ACCESS 0
#define FIRST_LAST_ACCESS 1
#define ADDITIONAL_BUFFER 1000000

/*********************************************/
// CS3223 - Data Structure declarations
typedef struct counter_info {
	uint64_t counter;
	slock_t counter_spinlock;
} counter_info;

typedef struct node {
	struct node* prev;
	struct node* next;
	int frame_id;
	uint64_t time_array[2];
	int sanity_check;
} node;

typedef struct info {
	struct node* head;
	struct node* tail;
	int size;
	slock_t linkedListInfo_spinlock;
} info;

static node* doubleLinkedList = NULL; 			//Make Global Declare it in InitializeStructure
static node* otherDoubleLinkedList = NULL;      // B2
static info* linkedListInfo = NULL;
static info* otherLinkedListInfo = NULL;       // B2
static counter_info* counterInfo = NULL;
node* search_for_frame(int desired_frame_id);
void delete_arbitrarily(int frame_id_for_deletion);
void insert_at_head(node* frame);
void move_to_head(node* frame);       // Case 1 - Called by StrategyAccessBuffer(..., false) in bufmgr_lru.c

//Pre-declare functions
void insert_into_b2(node* frame);
void move_to_head_b2(node* frame);
void delete_other_arbitrarily(int frame_id_for_deletion);
node* search_for_frame_b2(int desired_frame_id);
node* search_for_frame_before(int desired_frame_id);
node* search_for_frame_after(int desired_frame_id);
void update_time(node* frame);

/*********************************************/
// CS3223 - Function definitions

// Traverse through linkedListInfo for frame corresponding to some 'frame_id'
node* search_for_frame(int desired_frame_id) {
	//elog(LOG, "Searching for frame %d in B1", desired_frame_id);
	node* traversal_ptr;

	if (linkedListInfo->head == NULL) { 
		//elog(ERROR, "linkedListInfo is empty");
		//elog(LOG, "B1 is empty");
		return NULL; // Handle empty list case properly
	} else {
		traversal_ptr = linkedListInfo->head;

		while (traversal_ptr != NULL) {
			if (traversal_ptr->frame_id == desired_frame_id) {
				//elog(LOG, "Frame %d found in B1", desired_frame_id);
				return traversal_ptr;
			}

			traversal_ptr = traversal_ptr->next;
		}
	}

	//elog(LOG, "B1 is not empty but Frame %d not found in B1", desired_frame_id);
	return NULL; // Return NULL if frame_id not found
}

void delete_arbitrarily(int frame_id_for_deletion) {
	//elog(LOG, "Deleting frame %d from B1", frame_id_for_deletion);
	//log_linked_list(linkedListInfo);
 	node* frame_for_deletion = search_for_frame(frame_id_for_deletion);

	if (!frame_for_deletion) { // Handle case where frame is not found
		//elog(LOG, "Frame %d not found in B1", frame_id_for_deletion);
		return;
	}

	//log prev and next frame of frame_for_deletion, print null if either are null
	if (frame_for_deletion->prev && frame_for_deletion->next) {
		//elog(LOG, "Prev frame of frame %d: %d, Next frame of frame %d: %d", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion, frame_for_deletion->next->frame_id);
	} else if (frame_for_deletion->prev) {
		//elog(LOG, "Prev frame of frame %d: %d, Next frame of frame %d: NULL", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion);
	} else if (frame_for_deletion->next) {
		//elog(LOG, "Prev frame of frame %d: NULL, Next frame of frame %d: %d", frame_id_for_deletion, frame_id_for_deletion, frame_for_deletion->next->frame_id);
	} else {
		////elog(LOG, "Prev frame of frame %d: NULL, Next frame of frame %d: NULL", frame_id_for_deletion, frame_id_for_deletion);
	}

	// sanity check
	if (frame_for_deletion->sanity_check != 42069) {
		//elog(ERROR, "Sanity check failed for frame %d, sanity check value: %d and not 42069", frame_id_for_deletion, frame_for_deletion->sanity_check);
	}	

	if (frame_for_deletion == linkedListInfo->head) { // Correctly check and update head
		//elog(LOG, "if (frame_for_deletion == linkedListInfo->head) triggered");
		if (linkedListInfo->head->next) { // Check if there's a next node
			linkedListInfo->head = linkedListInfo->head->next;
			linkedListInfo->head->prev = NULL;
			} 
		else { // List becomes empty
			linkedListInfo->head = linkedListInfo->tail = NULL;
			}
		} 
	else if (frame_for_deletion == linkedListInfo->tail) { // Correctly check and update tail
			//elog(LOG, "if (frame_for_deletion == linkedListInfo->tail) triggered");
			linkedListInfo->tail = linkedListInfo->tail->prev;
			linkedListInfo->tail->next = NULL;
		} 
	else { // Node is in the middle
			//elog(LOG, "else triggered");
			frame_for_deletion->prev->next = frame_for_deletion->next;
			frame_for_deletion->next->prev = frame_for_deletion->prev;
		}
	//elog(LOG, "Successfully deleted frame %d from B1", frame_id_for_deletion);
	frame_for_deletion->next = NULL;
	frame_for_deletion->prev = NULL;
}

void insert_at_head(node* frame) { 
	// Update time_array
	update_time(frame);

	//elog(LOG, "Inserting frame %d into B1", frame->frame_id);

	frame->next = linkedListInfo->head; 
	if (linkedListInfo->head != NULL && linkedListInfo->tail != NULL) { // Check if list is not empty
		//elog(LOG, "Head of B1: %d", linkedListInfo->head->frame_id);
		// just log sanity check value
		//elog(LOG, "Sanity check value of head of B1: %d", linkedListInfo->head->sanity_check);

		linkedListInfo->head->prev = frame; 
	}

	linkedListInfo->head = frame; 

	if (linkedListInfo->tail == NULL) { // If list was empty, update tail as well
		//elog(LOG, "Tail of B1: NULL");
		linkedListInfo->tail = frame;
		frame->next = NULL;
	}

	frame->prev = NULL; // Set frame's prev to NULL
} 

void move_to_head(node* frame) { 
	delete_arbitrarily(frame->frame_id);
	delete_other_arbitrarily(frame->frame_id);
	insert_at_head(frame); 
}

// B2 - Function definitions

// Rank in B2(otherLinkedListInfo) is based on 2nd last accessed time i.e time_array[SECOND_LAST_ACCESS]
// Traverse through otherLinkedListInfo and check rank of frame corresponding by comparing time_array[SECOND_LAST_ACCESS]
// of other frames with the time_array[SECOND_LAST_ACCESS] of the frame to be inserted, and insert the frame at the correct position.
// The frame with the highest rank (largest time_array[SECOND_LAST_ACCESS]) will be at the head of the list.
// If insertion is successful, delete frame from linkedListInfo(original B1 list).
void insert_into_b2(node* frame) {
	//elog(LOG, "Inserting frame %d into B2", frame->frame_id);
	//elog(LOG, "Second last access time of frame %d: %lu", frame->frame_id, frame->time_array[SECOND_LAST_ACCESS]);
	//elog(LOG, "First last access time of frame %d: %lu", frame->frame_id, frame->time_array[FIRST_LAST_ACCESS]);
	node* traversal_ptr;
	//node* next_frame;
	node* prev_frame;

	// Update time array for frame
	update_time(frame);

	//elog(LOG, "managed to update time for frame %d", frame->frame_id);

	// delete frame from B2 if it exists first, then insert into B2 at the correct position
	delete_other_arbitrarily(frame->frame_id);

	//elog(LOG, "managed to delete frame from B2");

	delete_arbitrarily(frame->frame_id);

	//elog(LOG, "managed to delete frame from B1");

	if (otherLinkedListInfo->tail == NULL) { // If B2 is empty
		//elog(LOG, "B2 is empty");
		otherLinkedListInfo->head = otherLinkedListInfo->tail = frame;
		frame->prev = frame->next = NULL;
	} else {
		//elog(LOG, "B2 is not empty");
		traversal_ptr = otherLinkedListInfo->head;
		//elog(LOG, "Access time and frame_id of traversal_ptr: %lu, %d", traversal_ptr->time_array[SECOND_LAST_ACCESS], traversal_ptr->frame_id);

		while (traversal_ptr != NULL) {
			if (traversal_ptr->time_array[SECOND_LAST_ACCESS] <= frame->time_array[SECOND_LAST_ACCESS]) {
				//next_frame = traversal_ptr->next;
				prev_frame = traversal_ptr->prev;

				if (prev_frame) {
					prev_frame->next = frame;
				} else {
					otherLinkedListInfo->head = frame;
				}

				//elog(LOG, "Inserting frame %d into B2 at position %d", frame->frame_id, traversal_ptr->frame_id);

				frame->prev = prev_frame;
				frame->next = traversal_ptr;
				traversal_ptr->prev = frame;

				// Edge case - Insertion with one element only (HEAD & TAIL both pointing to the same element)
				// if (otherLinkedListInfo->head == otherLinkedListInfo->tail) {
				// 	otherLinkedListInfo->tail = traversal_ptr;
				// }

				return;
			}

			traversal_ptr = traversal_ptr->next;
		}

		// If frame has the lowest rank, insert at the end

		//elog(LOG, "Inserting frame %d into B2 at the end", frame->frame_id);
		
		otherLinkedListInfo->tail->next = frame;
		frame->prev = otherLinkedListInfo->tail;
		frame->next = NULL;
		otherLinkedListInfo->tail = frame;

		//elog(LOG, "Successfully inserted frame %d into B2 at the end", frame->frame_id);
	}
}

//Moves a frame in otherLinkedListInfo to the head of the list
void move_to_head_b2(node* frame) {
	//elog(LOG, "Moving frame %d to the head of B2", frame->frame_id);
	if (frame == otherLinkedListInfo->head) {
		return;
	}

	if (frame == otherLinkedListInfo->tail) {
		otherLinkedListInfo->tail = frame->prev;
		otherLinkedListInfo->tail->next = NULL;
	} else {
		frame->prev->next = frame->next;
		frame->next->prev = frame->prev;
	}

	frame->next = otherLinkedListInfo->head;
	frame->prev = NULL;
	otherLinkedListInfo->head->prev = frame;
	otherLinkedListInfo->head = frame;
}

void delete_other_arbitrarily(int frame_id_for_deletion) {
	//elog(LOG, "Deleting frame %d from B2", frame_id_for_deletion);
	node* frame_for_deletion = search_for_frame_b2(frame_id_for_deletion);
	//elog(LOG, "Frame %d found in B2 in delete_other_arbitrarily", frame_id_for_deletion);

	if (!frame_for_deletion) { // Handle case where frame is not found
		return;
	}

	if (frame_for_deletion == otherLinkedListInfo->head) { // Correctly check and update head
		//elog(LOG, "if (frame_for_deletion == otherLinkedListInfo->head) triggered in delete_other_arbitrarily");
		if (otherLinkedListInfo->head->next) { // Check if there's a next node
			otherLinkedListInfo->head = otherLinkedListInfo->head->next;
			otherLinkedListInfo->head->prev = NULL;
		} else { // List becomes empty
			otherLinkedListInfo->head = otherLinkedListInfo->tail = NULL;
		}
	} else if (frame_for_deletion == otherLinkedListInfo->tail) { // Correctly check and update tail
		//elog(LOG, "if (frame_for_deletion == otherLinkedListInfo->tail) triggered in delete_other_arbitrarily");
		otherLinkedListInfo->tail = otherLinkedListInfo->tail->prev;
		otherLinkedListInfo->tail->next = NULL;
	} else { // Node is in the middle
		//elog(LOG, "else triggered in delete_other_arbitrarily");
		//log check if prev or next is null
		if (frame_for_deletion->prev && frame_for_deletion->next) {
			//elog(LOG, "Prev frame of frame %d: %d, Next frame of frame %d: %d", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion, frame_for_deletion->next->frame_id);
		} else if (frame_for_deletion->prev) {
			//elog(LOG, "Prev frame of frame %d: %d, Next frame of frame %d: NULL", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion);
			//search for frame right after the frame with the frame to be deleted
			node* after_frame = search_for_frame_after(frame_id_for_deletion);
			frame_for_deletion->next = after_frame;
		} else if (frame_for_deletion->next) {
			//elog(LOG, "Prev frame of frame %d: NULL, Next frame of frame %d: %d", frame_id_for_deletion, frame_id_for_deletion, frame_for_deletion->next->frame_id);
			//search for frame right before the frame with the frame to be deleted
			node* before_frame = search_for_frame_before(frame_id_for_deletion);
			frame_for_deletion->prev = before_frame;
		} else {
			//elog(LOG, "Prev frame of frame %d: NULL, Next frame of frame %d: NULL", frame_id_for_deletion, frame_id_for_deletion);
		}
		///////////////////////////////////////
		if (frame_for_deletion->prev && frame_for_deletion->next) {
			//elog(LOG, "New Prev frame of frame %d: %d, New Next frame of frame %d: %d", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion, frame_for_deletion->next->frame_id);
		} else if (frame_for_deletion->prev) {
			//elog(LOG, "New Prev frame of frame %d: %d, New Next frame of frame %d: NULL", frame_id_for_deletion, frame_for_deletion->prev->frame_id, frame_id_for_deletion);
		} else if (frame_for_deletion->next) {
			//elog(LOG, "New Prev frame of frame %d: NULL, New Next frame of frame %d: %d", frame_id_for_deletion, frame_id_for_deletion, frame_for_deletion->next->frame_id);
		} else {
			//elog(LOG, "New Prev frame of frame %d: NULL, New Next frame of frame %d: NULL", frame_id_for_deletion, frame_id_for_deletion);
		}

		// if (otherLinkedListInfo->head != NULL)
		// 	//elog(LOG, "Head of B2: %d", otherLinkedListInfo->head->frame_id);
		// if (linkedListInfo->head != NULL)
		// 	//elog(LOG, "Head of B1: %d", linkedListInfo->head->frame_id);
		// // only log tail if it exists
		// if (otherLinkedListInfo->tail != NULL)
		// 	//elog(LOG, "Tail of B2: %d", otherLinkedListInfo->tail->frame_id);
		// if (linkedListInfo->tail != NULL)
		// 	//elog(LOG, "Tail of B1: %d", linkedListInfo->tail->frame_id);

		// elog(LOG, "I love CS3223!");	
		//log_linked_list(linkedListInfo);
		//log_linked_list_backwards(linkedListInfo);
		//log_b2_linked_list(otherLinkedListInfo);
		//log_b2_linked_list_backwards(otherLinkedListInfo);	

		frame_for_deletion->prev->next = frame_for_deletion->next;
		frame_for_deletion->next->prev = frame_for_deletion->prev;

		frame_for_deletion->next = NULL;
		frame_for_deletion->prev = NULL;
	}
}

// Search for frame in B2
node* search_for_frame_b2(int desired_frame_id) {
	node* traversal_ptr;

	if (otherLinkedListInfo->head == NULL) { 
		//elog(ERROR, "otherLinkedListInfo is empty");
		return NULL; // Handle empty list case properly
	} else {
		traversal_ptr = otherLinkedListInfo->head;
		//log head 
		// if (otherLinkedListInfo->head != NULL)
		// 	elog(LOG, "Head of B2: %d", otherLinkedListInfo->head->frame_id);
		// if (linkedListInfo->head != NULL)
		// 	elog(LOG, "Head of B1: %d", linkedListInfo->head->frame_id);
		// // only log tail if it exists
		// if (otherLinkedListInfo->tail != NULL)
		// 	elog(LOG, "Tail of B2: %d", otherLinkedListInfo->tail->frame_id);
		// if (linkedListInfo->tail != NULL)
		// 	elog(LOG, "Tail of B1: %d", linkedListInfo->tail->frame_id);

		// log_linked_list(linkedListInfo);
		// log_linked_list_backwards(linkedListInfo);
		// log_b2_linked_list(otherLinkedListInfo);
		// log_b2_linked_list_backwards(otherLinkedListInfo);		

		while (traversal_ptr != NULL) {
			if (traversal_ptr->frame_id == desired_frame_id) {
				return traversal_ptr;
			}

			traversal_ptr = traversal_ptr->next;
		}
	}

	return NULL; // Return NULL if frame_id not found
}

//search and return pointer to frame right before the frame with the desired frame_id in B2
node* search_for_frame_before(int desired_frame_id) {
	node* traversal_ptr;

	if (otherLinkedListInfo->head == NULL) { 
		return NULL; // Handle empty list case properly
	} else {
		traversal_ptr = otherLinkedListInfo->head;

		while (traversal_ptr != NULL && traversal_ptr->next != NULL) {
			if (traversal_ptr->next->frame_id == desired_frame_id) {
				return traversal_ptr;
			}

			traversal_ptr = traversal_ptr->next;
		}
	}

	return NULL; // Return NULL if frame_id not found
}

//search and return pointer to frame right after the frame with the desired frame_id, this has to search from the tail
node* search_for_frame_after(int desired_frame_id) {
	node* traversal_ptr;

	if (otherLinkedListInfo->tail == NULL) { 
		return NULL; // Handle empty list case properly
	} else {
		traversal_ptr = otherLinkedListInfo->tail;

		while (traversal_ptr != NULL && traversal_ptr->prev != NULL) {
			if (traversal_ptr->prev->frame_id == desired_frame_id) {
				
				return traversal_ptr;
			}

			elog(LOG, "traversal_ptr->frame_id: %d", traversal_ptr->frame_id);

			traversal_ptr = traversal_ptr->prev;
		}
	}

	return NULL; // Return NULL if frame_id not found
}

// Update time array depending if first or accessed again
void update_time(node* frame) {
	//acquire spinlock for counter
	SpinLockAcquire(&counterInfo->counter_spinlock);
	//elog(LOG, "Updating time for frame %d, with counter: %lu, frame first last access time: %lu, frame second last access time: %lu", frame->frame_id, counterInfo->counter, frame->time_array[FIRST_LAST_ACCESS], frame->time_array[SECOND_LAST_ACCESS]);
	bool not_first_update;
	//If both array values are zero then it is the first time the frame is being accessed
	if (frame->time_array[SECOND_LAST_ACCESS] == 0 && frame->time_array[FIRST_LAST_ACCESS] == 0) {
		//elog(LOG, "First update for frame %d", frame->frame_id);
		not_first_update = false;
	} else {
		//elog(LOG, "Not first update for frame %d", frame->frame_id);
		not_first_update = true;
	}
	if (not_first_update) {
		frame->time_array[SECOND_LAST_ACCESS] = frame->time_array[FIRST_LAST_ACCESS];
		frame->time_array[FIRST_LAST_ACCESS] = counterInfo->counter;
		//elog (LOG, "Updated second last access time to: %lu, and first last access time to: %lu", frame->time_array[SECOND_LAST_ACCESS], frame->time_array[FIRST_LAST_ACCESS]);
	} else {
		frame->time_array[FIRST_LAST_ACCESS] = counterInfo->counter;
		//elog (LOG, "Updated first last access time to: %lu", frame->time_array[FIRST_LAST_ACCESS]);
	}
	//release spinlock for counter
	SpinLockRelease(&counterInfo->counter_spinlock);
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

	int debug_limit = 0;

    while (current != NULL && debug_limit < 100 && current->frame_id != -1) {
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
		debug_limit++;
    }

    // Optionally, remove the last arrow " -> " for aesthetics
    if (offset > 4) {
        list_str[offset - 4] = '\0';
    }

    return list_str; // Caller is responsible for freeing the memory
}

// print list to string but backwards
char* print_list_to_string_backwards(info* linkedListInfo) {
	// Initial allocation for the string
	int str_size = 256; // Initial size, may need to increase depending on list size
	char* list_str = malloc(str_size * sizeof(char));
	if (!list_str) {
		// Handle memory allocation failure
		return NULL;
	}

	list_str[0] = '\0'; // Start with an empty string

	node* current = linkedListInfo->tail;
	int offset = 0; // Keep track of the number of characters written

	int debug_limit = 0;

	while (current != NULL && debug_limit < 100 && current->frame_id != -1) {
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

		current = current->prev;
		debug_limit++;
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
        //elog(LOG, "B1 LinkedList: %s", list_representation);
        free(list_representation);
    } else {
        elog(ERROR, "Failed to allocate memory for list representation");
    }
}

// void log_linked_list_backwards(info* linkedListInfo) {
// 	char* list_representation = print_list_to_string_backwards(linkedListInfo);
// 	if (list_representation) {
// 		elog(LOG, "B1 LinkedList Backwards: %s", list_representation);
// 		free(list_representation);
// 	} else {
// 		elog(ERROR, "Failed to allocate memory for list representation");
// 	}
// }

// void log_b2_linked_list(info* linkedListInfo) {
// 	char* list_representation = print_list_to_string(linkedListInfo);
// 	if (list_representation) {
// 		elog(LOG, "B2 LinkedList: %s", list_representation);
// 		free(list_representation);
// 	} else {
// 		elog(ERROR, "Failed to allocate memory for b2 list representation");
// 	}
// }

// void log_b2_linked_list_backwards(info* linkedListInfo) {
// 	char* list_representation = print_list_to_string_backwards(linkedListInfo);
// 	if (list_representation) {
// 		elog(LOG, "B2 LinkedList Backwards: %s", list_representation);
// 		free(list_representation);
// 	} else {
// 		elog(ERROR, "Failed to allocate memory for b2 list representation");
// 	}
// }

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
	//log entered function for buf_id and delete is true or false
	//elog(LOG, "Entered StrategyAccessBuffer for buffer %d, delete: %d", buf_id, delete);
	//acquire spinlock for counter
	SpinLockAcquire(&counterInfo->counter_spinlock);
	counterInfo->counter++;
	//release spinlock for counter
	SpinLockRelease(&counterInfo->counter_spinlock);
	//elog(LOG, "Incremented Counter at StrategyAccessBuffer to: %lu", counterInfo->counter);
	node* frame;
	if (delete) {
		//elog(LOG, "entered delete of strategy access buffer");
        SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "Spinlock acquired for linkedListInfo");
		////elog(LOG, "SpinLOCK A");
		//log_linked_list(linkedListInfo);
		//log_b2_linked_list(otherLinkedListInfo);

        delete_arbitrarily(buf_id);
		// doubleLinkedList[buf_id].next = NULL;
		// doubleLinkedList[buf_id].prev = NULL;
		// doubleLinkedList[buf_id].frame_id = -1;
		// doubleLinkedList[buf_id].time_array[0] = 0;
		// doubleLinkedList[buf_id].time_array[1] = 0;
		// doubleLinkedList[buf_id].sanity_check = 42069;

        SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
		////elog(LOG, "SpinRELEASE A");
		//log_linked_list(linkedListInfo);
		//log_b2_linked_list(otherLinkedListInfo);

		// B2, did not exist in B1 so we have to delete from B2 now
		SpinLockAcquire(&otherLinkedListInfo->linkedListInfo_spinlock);

		delete_other_arbitrarily(buf_id);
		// otherDoubleLinkedList[buf_id].next = NULL;
		// otherDoubleLinkedList[buf_id].prev = NULL;
		// otherDoubleLinkedList[buf_id].frame_id = -1;
		// otherDoubleLinkedList[buf_id].time_array[0] = 0;
		// otherDoubleLinkedList[buf_id].time_array[1] = 0;
		// otherDoubleLinkedList[buf_id].sanity_check = 42069;

		SpinLockRelease(&otherLinkedListInfo->linkedListInfo_spinlock);
    } else {
		//elog(LOG, "entered else of strategy access buffer");
		SpinLockAcquire(&linkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "Spinlock acquired for linkedListInfo");
		SpinLockAcquire(&otherLinkedListInfo->linkedListInfo_spinlock);
		//elog(LOG, "Spinlock acquired for otherLinkedListInfo");
		////elog(LOG, "SpinLOCK B");
		//log_linked_list(linkedListInfo);
		//log_b2_linked_list(otherLinkedListInfo);

		//Search for frame in B1
		frame = search_for_frame(buf_id);
		if (frame) {			
			insert_into_b2(frame);			
		} else {
			// Frame does not exist in B1, so we have to search for it in B2
			frame = search_for_frame_b2(buf_id);

			if (frame) {
				//elog(LOG, "Inserting frame %d into B2 again(to put it at new postion) from StrategyAccessBuffer if statement", frame->frame_id);
				insert_into_b2(frame);
			} else{
				node* new_frame = &doubleLinkedList[buf_id];
				new_frame->frame_id = buf_id;
				new_frame->time_array[SECOND_LAST_ACCESS] = 0;
				new_frame->time_array[FIRST_LAST_ACCESS] = 0;
				new_frame->sanity_check = 42069;
				//elog(LOG, "Inserting frame %d into B1 from StrategyAccessBuffer else statement", new_frame->frame_id);
				move_to_head(new_frame);
			}
		}

		SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
		SpinLockRelease(&otherLinkedListInfo->linkedListInfo_spinlock);
		////elog(LOG, "SpinRELEASE B");
		//log_linked_list(linkedListInfo);
		//log_b2_linked_list(otherLinkedListInfo);
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
	//elog(LOG, "Entered StrategyGetBuffer");
	//acquire spinlock for counter
	SpinLockAcquire(&counterInfo->counter_spinlock);
	counterInfo->counter++;
	//release spinlock for counter
	SpinLockRelease(&counterInfo->counter_spinlock);

	//elog(LOG, "Incremented Counter at StrategyGetBuffer to: %lu", counterInfo->counter);

	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */

	// CS3223
	node* traversal_frame;
	int fetched_frame_id;
	node *fetched_frame;

	// B2
	node* otherTraversal_frame;
	int other_frame_id;
	node *other_fetched_frame;

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
				////elog(LOG, "Case 2");
				//log_linked_list(linkedListInfo);
				//log_b2_linked_list(otherLinkedListInfo);
			
				// AddBufferToRing(strategy, buf);
				//CS3223: Add buffer to the head of the linked list
				StrategyAccessBuffer(buf->buf_id, false);                      // Case 2
				// SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
				////elog(LOG, "Case 2");
				//log_linked_list(linkedListInfo);
				//log_b2_linked_list(otherLinkedListInfo);
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
	SpinLockAcquire(&otherLinkedListInfo->linkedListInfo_spinlock);
	////elog(LOG, "SpinLOCK Case 3");
	//log_linked_list(linkedListInfo);
	//log_b2_linked_list(otherLinkedListInfo);
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
			//SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);    // Release the DLL spinlock we acquired before for(;;)
			
			//This is the case where B1 is empty (Original linked list is empty or has no unpinned frames)
			//So we have to scan through B2 now to find and unpinned frame to evict
			

			otherTraversal_frame = otherLinkedListInfo->tail;
			for (;;)
			{
				if (otherTraversal_frame == NULL) {
					// We must have traversed the entire list, or the list is empty
					// i.e All buffers are pinned

					// Thus, the result should be similar to Clock Policy (where all frames are pinned, none can be evicted)
					// We follow their method there
					otherTraversal_frame = otherLinkedListInfo->tail;				          // Reset traversal to the tail
					SpinLockRelease(&otherLinkedListInfo->linkedListInfo_spinlock);    // Release the DLL spinlock we acquired before for(;;)
					SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock); 
					elog(ERROR, "no unpinned buffers available");
				}

				other_frame_id = otherTraversal_frame->frame_id;				
				buf = GetBufferDescriptor(other_frame_id);
				local_buf_state = LockBufHdr(buf);

				if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
				{
					// Found a usable buffer
					// AddBufferToRing(strategy, other_fetched_frame);
					////elog(LOG, "SpinRELEASE Case 3 else");
					//log_linked_list(linkedListInfo);
					//log_b2_linked_list(otherLinkedListInfo);

					other_fetched_frame = search_for_frame_b2(other_frame_id);
					//log insert at head called from strategygetbuffer
					//elog(LOG, "Inserting frame %d into B1 from strategygetbuffer", other_fetched_frame->frame_id);
					delete_other_arbitrarily(other_fetched_frame->frame_id);
					otherDoubleLinkedList[other_fetched_frame->frame_id].next = NULL;
					otherDoubleLinkedList[other_fetched_frame->frame_id].prev = NULL;
					otherDoubleLinkedList[other_fetched_frame->frame_id].frame_id = -1;
					otherDoubleLinkedList[other_fetched_frame->frame_id].time_array[0] = 0;
					otherDoubleLinkedList[other_fetched_frame->frame_id].time_array[1] = 0;
					otherDoubleLinkedList[other_fetched_frame->frame_id].sanity_check = 42069;
					move_to_head(other_fetched_frame);
					SpinLockRelease(&otherLinkedListInfo->linkedListInfo_spinlock);
					SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock); 

					*buf_state = local_buf_state;
					return buf;
				}
				UnlockBufHdr(buf, local_buf_state);
				otherTraversal_frame = otherTraversal_frame -> prev;
			}

		}

		fetched_frame_id = traversal_frame->frame_id;
		buf = GetBufferDescriptor(fetched_frame_id);
		local_buf_state = LockBufHdr(buf);

		//elog(NOTICE, "fetched_frame is %d", fetched_frame_id);
		//elog(NOTICE, "RC is %d", BUF_STATE_GET_REFCOUNT(local_buf_state));

		// Check if the frame_id will be valid below...
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			////elog(LOG, "Entered: if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0) ");
			// if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			// {
			// 	local_buf_state -= BUF_USAGECOUNT_ONE;
			// 	trycounter = NBuffers;
			// }
			// else
			//{
				/* Found a usable buffer */
				// if (strategy != NULL) {
				// 	//elog(LOG, "Non-default strategy found a buffer");
				// }
				// AddBufferToRing(strategy, buf);

			fetched_frame = search_for_frame(fetched_frame_id);
			delete_other_arbitrarily(fetched_frame_id);
			// otherDoubleLinkedList[fetched_frame_id].next = NULL;
			// otherDoubleLinkedList[fetched_frame_id].prev = NULL;
			// otherDoubleLinkedList[fetched_frame_id].frame_id = -1;
			// otherDoubleLinkedList[fetched_frame_id].time_array[0] = 0;	
			// otherDoubleLinkedList[fetched_frame_id].time_array[1] = 0;
			// otherDoubleLinkedList[fetched_frame_id].sanity_check = 42069;			
			move_to_head(fetched_frame);

			SpinLockRelease(&linkedListInfo->linkedListInfo_spinlock);
			SpinLockRelease(&otherLinkedListInfo->linkedListInfo_spinlock);

			////elog(LOG, "SpinRELEASE Case 3 else");
			//log_linked_list(linkedListInfo);
			//log_b2_linked_list(otherLinkedListInfo);
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
		// 	//elog(LOG, "SpinRELEASE Case 3 elseif");
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
	//elog(LOG, "Entered StrategyFreeBuffer");
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	////elog(LOG, "SpinLOCK Case 4");
	//log_linked_list(linkedListInfo);
	//log_b2_linked_list(otherLinkedListInfo);

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
	////elog(LOG, "SpinRELEASE Case 4");
	//log_linked_list(linkedListInfo);
	//log_b2_linked_list(otherLinkedListInfo);
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
	size = add_size(size, sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER));

	//Size of the control information of double link list
	size = add_size(size, sizeof(info));

	// CS3223: Allocate size for our data structures in B2
	size = add_size(size, sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER));

	//Size of the control information of B2 double link list
	size = add_size(size, sizeof(info));

	//Size of the counter info;
	size = add_size(size, sizeof(counter_info));

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

	bool is_other_dll_success = false;
	bool is_other_link_list_info_success = false;

	bool is_counter_info_success = false;

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
														sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER),
														&is_dll_success);
	
	// Intialization for B2
	otherLinkedListInfo = (info *)ShmemInitStruct("Other Link List Info",
												sizeof(info),
												&is_other_link_list_info_success);

	// Double Linked List itself
	otherDoubleLinkedList = (node *)ShmemInitStruct("Other Double Link List",
														sizeof(node) * (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER),
														&is_other_dll_success);

	// Counter Info
	counterInfo = (counter_info *)ShmemInitStruct("Counter Info",
												sizeof(counter_info),
												&is_counter_info_success);													

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
		//log what is MBiffers and what is NUM_BUFFER_PARTITIONS
		//elog(LOG, "NBuffers: %d and NUM_BUFFER_PARTITIONS: %d", NBuffers, NUM_BUFFER_PARTITIONS);

		// We used NBuffers + NUM_BUFFER_PARTITIONS in the original size of the doubleLinkedList
		for (int i = 0; i < (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER); i++) {
			doubleLinkedList[i].prev = NULL;
			doubleLinkedList[i].next = NULL;
			doubleLinkedList[i].frame_id = -1;
			doubleLinkedList[i].time_array[0] = 0;
			doubleLinkedList[i].time_array[1] = 0;
			doubleLinkedList[i].sanity_check = 12345;
    	}

		//Check freshly initialized linked list linkedListInfo->head frame id
		//elog(LOG, "freshly initialized linkedListInfo->head frame id: %d", linkedListInfo->head->frame_id);
		//compare with doubleLinkedList[0] head frame id
		//elog(LOG, "doubleLinkedList[0] head frame id: %d", doubleLinkedList[0].frame_id);

	} else
		Assert(!init);

	// CS3223: Intialize our OTHER DLL Data Structure
	if (!is_other_dll_success && !is_other_link_list_info_success) { //Initiate our Double Link List Data Structure here for B2
		Assert (init);
		SpinLockInit(&otherLinkedListInfo->linkedListInfo_spinlock);

		otherLinkedListInfo->tail = NULL;
		otherLinkedListInfo->size = 0;
		otherLinkedListInfo->head = otherDoubleLinkedList;
		for (int i = 0; i < (NBuffers + NUM_BUFFER_PARTITIONS + ADDITIONAL_BUFFER); i++) {
			otherDoubleLinkedList[i].prev = NULL;
			otherDoubleLinkedList[i].next = NULL;
			otherDoubleLinkedList[i].frame_id = -1;
			otherDoubleLinkedList[i].time_array[0] = 0;
			otherDoubleLinkedList[i].time_array[1] = 0;
			otherDoubleLinkedList[i].sanity_check = 42069;
		}

	} else
		Assert(!init);
	
	// CS3223: Intialize our Counter Info Data Structure
	if (!is_counter_info_success) { //Initiate our Counter Info Data Structure here
		Assert (init);
		SpinLockInit(&counterInfo->counter_spinlock);
		counterInfo->counter = 0;
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
