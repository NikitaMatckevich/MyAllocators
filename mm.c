/*
 * mm-naive.c - The fastest, least memory-efficient malloc package.
 * 
 * In this naive approach, a block is allocated by simply incrementing
 * the brk pointer.  A block is pure payload. There are no headers or
 * footers.  Blocks are never coalesced or reused. Realloc is
 * implemented directly using mm_malloc and mm_free.
 *
 * NOTE TO STUDENTS: Replace this header comment with your own header
 * comment that gives a high level description of your solution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "ateam",
    /* First member's full name */
    "Nikita Matckevich",
    /* First member's email address */
    "nikita.matckevich@ensta-paris.fr",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8
/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7)

#define SIZE_T_SIZE  (sizeof(size_t))
#define PTR_T_SIZE   (sizeof(void*))

// block types
#define FREE 0
#define OCCUPIED 1
#define NO_MATTER 2

// returns the address that lies in len bytes before or after ptr
#define OFFSET(ptr, len) (void*)((char*)(ptr) + (len))

size_t  nb_components;
size_t* min_block_sizes;
void**  linked_components;
void*   blocks;

void mm_check(void);

// delete element from free block queue, p is a pointer to the very beginning
// of the block
static void delete_from_queue(void* p) {

  void** to_prev = (void**)OFFSET(p, SIZE_T_SIZE);
  void** to_next = (void**)OFFSET((void*)to_prev, PTR_T_SIZE);
  
  size_t i = 0;
  size_t len = *(size_t*)p;
  while (i < nb_components-1 && len > min_block_sizes[i]) {
    ++i;
  }

  if (*to_prev == NULL) {
    linked_components[i] = *to_next;
  } else {
    void** next_of_prev = (void**)OFFSET(*to_prev, PTR_T_SIZE);
    *next_of_prev = *to_next;
  }

  if (*to_next != NULL) {
    void** prev_of_next = (void**)OFFSET(*to_next, -PTR_T_SIZE);
    *prev_of_next = *to_prev;
  }
}

// add element to the queue, p is a pointer to the very beginning of the block
static void add_to_queue(void* p) {

  size_t len = *(size_t*)p;
  
  void** to_prev = (void**)OFFSET(p, SIZE_T_SIZE);
  void** to_next = (void**)OFFSET((void*)to_prev, PTR_T_SIZE);
  
  size_t i = 0;
  while (i < nb_components-1 && len > min_block_sizes[i]) {
    ++i;
  }

  *to_prev = NULL;
  *to_next = linked_components[i];
  linked_components[i] = (void*)to_next;

  if (*to_next != NULL) {
    void** prev_of_next = (void**)OFFSET(*to_next, -PTR_T_SIZE);
    *prev_of_next = (void*)to_prev;
  }
}

// finder for explicit lists
// len = 2*SIZE_T_SIZE + max(size of payload, 2*PTR_T_SIZE)
static void* find_block(size_t len)
{
  void* res = NULL;
  size_t i = 0;
  while (i < nb_components - 1 && len > min_block_sizes[i]) {
    ++i;
  }

  while (i < nb_components && res == NULL) {

    void* p = linked_components[i];
    size_t dist = mem_heapsize();
    while (p != NULL) {
      size_t s = *(size_t*)OFFSET(p, -SIZE_T_SIZE - PTR_T_SIZE);
      if (((s & 1) == FREE) && (s >= len) && (dist > s - len)) {
        res = p;
        dist = s - len;
        if (dist == 0)
          break;
      }
      p = *(void**)p;
    }
    i++;
  }

  if (res != NULL) {
    res = OFFSET(res, -SIZE_T_SIZE - PTR_T_SIZE);
  }

  return res;
}

// free block pointed by *p, change the *p value in case of coalescing
static void free_block(void** p) {

  size_t* bbeg = (size_t*)(*p);
  *bbeg = *bbeg & -2;

  size_t* bend = (size_t*)OFFSET(*p, *bbeg - SIZE_T_SIZE);
  *bend = *bend & -2;

  size_t bsize = *bend;

  if (OFFSET((void*)bend, SIZE_T_SIZE) < mem_heap_hi()) {
    size_t* next = (size_t*)OFFSET((void*)bend, SIZE_T_SIZE);
    if ((*next & 1) == FREE) {
      bsize += *next;
      bend = (size_t*)OFFSET((void*)bend, *next);
      delete_from_queue((void*)next);
    }
  }

  if ((void*)bbeg > blocks) {
    size_t* prev = (size_t*)OFFSET((void*)bbeg, -SIZE_T_SIZE);
    if ((*prev & 1) == FREE) {
      bsize += *prev;
      bbeg = (size_t*)OFFSET((void*)bbeg, -(*prev));
      delete_from_queue((void*)bbeg);
    }
  }
  
  *bbeg = bsize;
  *bend = bsize;
  
  *p = (void*)bbeg;
}

// occupy the block in the heap, called by malloc
static void occupy_block(void* p, size_t len) {
     
  size_t pload_threshold = 2*(SIZE_T_SIZE + PTR_T_SIZE);
  
  size_t* bbeg = (size_t*)p;
  size_t old_size = *bbeg;
 
  if (old_size - len < pload_threshold) {
    len = old_size;
  }

  size_t* bend = (size_t*)OFFSET(p, len - SIZE_T_SIZE);
  *bbeg = len | 1;
  *bend = len | 1;
  
  if (old_size - len >= pload_threshold) {
    size_t resid_len = old_size - len;
    size_t* resid_beg = (size_t*)OFFSET(p, len);
    size_t* resid_end = (size_t*)OFFSET(p, old_size - SIZE_T_SIZE);
    *resid_beg = resid_len;
    *resid_end = resid_len;
    add_to_queue((void*)resid_beg);
  }
}

// adjust heap size if needed
static void* adjust_heap(size_t size)
{
  size_t adjust_size = size;
  size_t last_size = *(size_t*)OFFSET(mem_heap_hi(), -SIZE_T_SIZE + 1);
  if ((last_size & 1) == FREE) {
    adjust_size -= last_size;
  } else {
    last_size = 0;
  }

  void* adjust = mem_sbrk(adjust_size);
  if (adjust == NULL) {
    printf("cannot adjust heap no more\n");
    printf("\theap size = %d", mem_heapsize());
    exit(8);
  }
  void* adjust_end = OFFSET(adjust, adjust_size - SIZE_T_SIZE);

  *(size_t*)adjust = adjust_size;
  *(size_t*)adjust_end = adjust_size;

  free_block(&adjust);
  
  return adjust;
}

// print LIFO queues of free blocks
static void print_linked_components(void) {
  for (size_t i = 0; i < nb_components; ++i) {
    void* curr;
    void* prev;

    curr = linked_components[i];
    prev = NULL;
    printf("[list %i]\n", i);
    printf("\tforward:\n");
    while (curr != NULL) {
      prev = curr;
      curr = *(void**)curr;
      printf("\t\t%p --> %p\n", prev, curr);
    }

    if (prev != NULL) {
      curr = OFFSET(prev, -PTR_T_SIZE);
      prev = NULL;
    }
    printf("\tbackward:\n");
    while (curr != NULL) {
      prev = curr;
      curr = *(void**)curr;
      printf("\t\t%p --> %p\n", prev, curr);
    }
  }
}

static int check_bounds(void* p, int status)
{
  size_t* bbeg = (size_t*)p;
  size_t* bend = (size_t*)OFFSET(bbeg, (*bbeg & -2) - SIZE_T_SIZE);
  if (*bbeg != *bend) {
    printf("left and right block sizes aren't equal\n");
    return 0;
  }

  if ((status != NO_MATTER) && ((*bbeg & 1) != (size_t)status)) {
    printf("block in free list marked as occupied\n");
    return 0;
  }

  if ((*bbeg & 1) == 0) {
    if ((void*)bbeg > blocks) {
      size_t* prev_end = (size_t*)OFFSET(bbeg, -SIZE_T_SIZE);
      if ((*prev_end & 1) == FREE) {
        printf("consecutive blocks escaped coalescing\n");
        return 0;
      }
    }
    if (OFFSET(bend, SIZE_T_SIZE) < mem_heap_hi()) {
      size_t* next_beg = (size_t*)OFFSET(bend,  SIZE_T_SIZE);
      if ((*next_beg & 1) == FREE) {
        printf("consecutive blocks escaped coalescing\n");
        return 0;
      }
    }
  }
  return 1;
}

static int check_valid_address(void* p)
{
  if ((p < blocks) || (p > mem_heap_hi())) {
    printf("pointer doesn't point to the heap region\n");
    printf("\taddress = %p\n", p);
    printf("\theap region = [%p, %p]", blocks, mem_heap_hi());
    return 0;
  }
  return 1;
}

static int check_block_size(size_t i, size_t len) {
  if ((i != nb_components-1 && len > min_block_sizes[i]) ||
      (i != 0 && len < min_block_sizes[i-1])) {
    printf("block is not in the right list\n");
    printf("\treal block size = %d\n", len);
    size_t pload_threshold = 2*SIZE_T_SIZE;
    pload_threshold += (SIZE_T_SIZE > 2*PTR_T_SIZE)? SIZE_T_SIZE : 2*PTR_T_SIZE;
    size_t min = (i == 0) ? pload_threshold : min_block_sizes[i-1];
    size_t max = (i == nb_components-1) ? (unsigned int)(-1) : min_block_sizes[i];
    printf("\texpected size range = [%d, %d]\n", min, max);
    return 0;
  }
  return 1;
}

static void* forward_iterations(void* s, size_t i) {
  void* prev = NULL;

  for (void* curr = s; curr != NULL; curr = *(void**)curr) {
    
    void* bbeg = OFFSET(curr, -PTR_T_SIZE - SIZE_T_SIZE);

    if (check_block_size(i, *(size_t*)bbeg) == 0)
      return NULL;
    
    void** to_prev = (void**)OFFSET(curr, -PTR_T_SIZE);
    if (prev != *to_prev) {
      printf("free block doesn't point to previous block in list %d\n", i);
      return NULL;
    }
     
    prev = (void*)to_prev;
  }

  return prev;
}

static void* backward_iterations(void* s) {
  void* next = NULL;
  for (void* curr = s; curr != NULL; curr = *(void**)curr) {

    if (check_valid_address(curr) == 0)
      return NULL;

    void** to_next = (void**)OFFSET(curr, PTR_T_SIZE);
    if (next != *to_next) {
      printf("free block doesn't point to next block in list\n");
      return NULL;
    }
 
    next = (void*)to_next;
  }

  return next;
}

static void check_implicit_heap(void)
{
  for (void* p = blocks; p < mem_heap_hi(); p = OFFSET(p, (*(size_t*)p & -2)))
  {
    if (!(check_valid_address(p) && check_bounds(p, NO_MATTER))) {
      printf("address = %p\n", p);
      exit(8);
    }
  }
}

static int check_free_lists(void)
{ 
  for (size_t i = 0; i < nb_components; ++i) {
    void* s = linked_components[i];
    void* f = forward_iterations(s, i);
    void* r = backward_iterations(f);
    if ((s != NULL) &&  (s != r))
    {
      printf("iterations do not return to blocks point\n");
      printf("STARTING POINT: %p\n", s);
      printf("RETURN POINT: %p\n", r);
      return 0;
    }
  }
  return 1;
}

void mm_check()
{
  check_implicit_heap();
  check_free_lists();
}

/* mm_init - initialize the malloc package. */
int mm_init(void)
{
  size_t mps = mem_pagesize();

  if (mem_sbrk(mps + SIZE_T_SIZE) == NULL) {
    printf("sbrk cannot adjust heap during initialization\n");
    exit(8);
  }
  void* lo_heap = mem_heap_lo();
  void* hi_heap = OFFSET(mem_heap_hi(), -SIZE_T_SIZE + 1);

  nb_components = 3;
  size_t offset_block_sz = (nb_components-1) * SIZE_T_SIZE;
  size_t offset_comps = nb_components * PTR_T_SIZE;
  
  min_block_sizes = (size_t*)lo_heap;
  linked_components = (void**)OFFSET(lo_heap, offset_block_sz);
  for (size_t i = 0; i < nb_components - 1; ++i) {
    min_block_sizes[i] = mps + (i - nb_components/2)*mps/4;
    linked_components[i] = NULL;
  }
  linked_components[nb_components-1] = NULL;

  size_t offset = offset_block_sz + offset_comps;
  offset = ALIGN(offset + SIZE_T_SIZE) - SIZE_T_SIZE;

  blocks = OFFSET(lo_heap, offset);
  *(size_t*)blocks  = mem_heapsize() - offset;
  *(size_t*)hi_heap = mem_heapsize() - offset;

  add_to_queue(blocks);

  return 0;
}

/* 
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void* mm_malloc(size_t size)
{
  size_t newsize;
  void* p;

  newsize = (size > 2*PTR_T_SIZE) ? size : 2*PTR_T_SIZE;
  newsize = ALIGN(newsize) + 2*SIZE_T_SIZE;
  p = find_block(newsize);
  
  if (p == NULL) {
    p = adjust_heap(newsize);
  } else {
    delete_from_queue(p); 
  }
 
  occupy_block(p, newsize);  
  return OFFSET(p, SIZE_T_SIZE);
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *p)
{
  void* bbeg = OFFSET(p, -SIZE_T_SIZE);
  if ((*(size_t*)bbeg & 1) == FREE) {
    printf("double free or corruption\n");
    exit(8);
  }
  free_block(&bbeg);
  add_to_queue(bbeg);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void* mm_realloc(void *ptr, size_t size)
{
  if ((ptr != NULL) && (size > 0)) {
  
    size_t* bbeg = (size_t*)OFFSET(ptr, -SIZE_T_SIZE);
    size_t oldsize = *bbeg & -2;
    size_t newsize = (size > 2*PTR_T_SIZE) ? size : 2*PTR_T_SIZE;
    newsize = ALIGN(newsize) + 2*SIZE_T_SIZE;

    void* resid_beg = OFFSET(bbeg, oldsize);
    size_t adjusted = 0;
    if ((resid_beg <= mem_heap_hi()) && ((*(size_t*)resid_beg & 1) == FREE))
    {
      adjusted = *(size_t*)resid_beg;
    }

    if (oldsize + adjusted >= newsize) {

      if (adjusted > 0) {
        delete_from_queue(resid_beg);
      }
      *bbeg = oldsize + adjusted;
      occupy_block((void*)bbeg, newsize);
      return ptr;

    } else {

      void* newptr = mm_malloc(size);
      if (size < oldsize) {
        oldsize = size;
      }
      memcpy(newptr, ptr, oldsize);
      mm_free(ptr);
      return newptr;
    }
  }
  if (ptr == NULL)
    return mm_malloc(size);
  if (size == 0)
    mm_free(ptr);
  return NULL;
}
