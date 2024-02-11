#include <stddef.h> 
#include <stdlib.h> // For malloc
#include <stdio.h> 

/*********************************************/

typedef struct node {
 struct node* prev;
 struct node* next;
 int frame_id;
} node;

typedef struct DLL {
 struct node* head;
 struct node* tail;
 int size;
 //slock_t doubleLinkedList_spinlock;
} DLL;

static DLL doubleLinkedList;

// Traverse through doubleLinkedList for frame corresponding to some 'frame_id'
node* search_for_frame(int desired_frame_id) {
 node* traversal_ptr;

 if (doubleLinkedList.head == NULL) { // Use '.' to access members of doubleLinkedList
  //elog(ERROR, "doubleLinkedList is empty");
  return NULL; // Handle empty list case properly
 } else {
  traversal_ptr = doubleLinkedList.head;

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
 node* frame_for_deletion = search_for_frame(frame_id_for_deletion); // Corrected function call

 if (!frame_for_deletion) { // Handle case where frame is not found
  return;
 }

 if (frame_for_deletion == doubleLinkedList.head) { // Correctly check and update head
  if (doubleLinkedList.head->next) { // Check if there's a next node
   doubleLinkedList.head = doubleLinkedList.head->next;
   doubleLinkedList.head->prev = NULL;
  } else { // List becomes empty
   doubleLinkedList.head = doubleLinkedList.tail = NULL;
  }
 } else if (frame_for_deletion == doubleLinkedList.tail) { // Correctly check and update tail
  doubleLinkedList.tail = doubleLinkedList.tail->prev;
  doubleLinkedList.tail->next = NULL;
 } else { // Node is in the middle
  frame_for_deletion->prev->next = frame_for_deletion->next;
  frame_for_deletion->next->prev = frame_for_deletion->prev;
 }

 // Free the node if necessary
 // free(frame_for_deletion);
}

void insert_at_head(node* frame) { 
 frame->next = doubleLinkedList.head; 
 if (doubleLinkedList.head != NULL) { // Check if list is not empty
  doubleLinkedList.head->prev = frame; 
 }
 doubleLinkedList.head = frame; 
 if (doubleLinkedList.tail == NULL) { // If list was empty, update tail as well
  doubleLinkedList.tail = frame;
 }
 frame->prev = NULL; // Set frame's prev to NULL
} 

void move_to_head(node* frame) { 
 delete_arbitrarily(frame->frame_id); // Correctly pass frame_id
 insert_at_head(frame); 
}

int main(void) {
    // Allocate and initialize a few nodes
    node nodes[3]; // For simplicity, using static allocation

    // Initialize nodes with frame_id values
    for (int i = 0; i < 3; ++i) {
        nodes[i].frame_id = i;
        nodes[i].next = NULL;
        nodes[i].prev = NULL;
    }

    // Insert nodes at the head of the list and print the list after each insertion
    printf("Inserting nodes at the head:\n");
    for (int i = 0; i < 3; ++i) {
        insert_at_head(&nodes[i]);
        node* current = doubleLinkedList.head;
        printf("List after insertion: ");
        while (current != NULL) {
            printf("%d ", current->frame_id);
            current = current->next;
        }
        printf("\n");
    }

    // Move the last node to the head and print the list
    printf("Moving last node to the head:\n");
    move_to_head(&nodes[0]); // Moving the first inserted node (which is now last) to the head
    node* current = doubleLinkedList.head;
    printf("List after moving to head: ");
    while (current != NULL) {
        printf("%d ", current->frame_id);
        current = current->next;
    }
    printf("\n");

    // Delete a node and print the list
    printf("Deleting a node from the list:\n");
    delete_arbitrarily(nodes[1].frame_id); // Delete the node with frame_id 1
    current = doubleLinkedList.head;
    printf("List after deletion: ");
    while (current != NULL) {
        printf("%d ", current->frame_id);
        current = current->next;
    }
    printf("\n");

    return 0;
}
