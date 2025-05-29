#pragma once
#include <cstddef>

struct buddy;

/* Returns the size of a buddy required to manage a block of the specified size */
size_t buddy_sizeof(size_t memory_size);

/*
 * Returns the size of a buddy required to manage a block of the specified size
 * using a non-default alignment.
 */
size_t buddy_sizeof_alignment(size_t memory_size, size_t alignment);

/* Initializes a binary buddy memory allocator at the specified location */
struct buddy *buddy_init(unsigned char *at, unsigned char *main, size_t memory_size);

/* Initializes a binary buddy memory allocator at the specified location using a non-default alignment */
struct buddy *buddy_init_alignment(unsigned char *at, unsigned char *main, size_t memory_size, size_t alignment);

/*
 * Initializes a binary buddy memory allocator embedded in the specified arena.
 * The arena's capacity is reduced to account for the allocator metadata.
 */
struct buddy *buddy_embed(unsigned char *main, size_t memory_size);

/*
 * Initializes a binary buddy memory allocator embedded in the specified arena using a non-default alignment.
 * The arena's capacity is reduced to account for the allocator metadata.
 */
struct buddy *buddy_embed_alignment(unsigned char *main, size_t memory_size, size_t alignment);

/* Use the specified buddy to allocate memory. */
void *buddy_malloc(struct buddy *buddy, size_t requested_size);

/* Use the specified buddy to deallocate memory. */
void buddy_dealloc(struct buddy *buddy, void *ptr);

/* Use the specified buddy to reallocate a memory block. */
void *buddy_realloc(struct buddy *buddy, void *ptr, size_t requested_size);

/* Prints the buddy allocator tree */
void buddy_debug(struct buddy *buddy);