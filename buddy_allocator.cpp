#include "buddy_allocator.h" 
#include <cstdint>
#include <climits>
#include <cstring>
#include <cstdio>

#ifndef BUDDY_ALLOC_ALIGN
#define BUDDY_ALLOC_ALIGN (sizeof(size_t) * CHAR_BIT)
#endif

#ifndef BUDDY_ALIGNOF
#define BUDDY_ALIGNOF(x) alignof(x)
#endif

#ifndef BUDDY_PRINTF
#define BUDDY_PRINTF printf
#endif

/*
 * A binary buddy memory allocator
 */

struct buddy {
    size_t memory_size;
    size_t alignment;
    union {
        unsigned char *main;
        ptrdiff_t main_offset;
    } arena;
    size_t buddy_flags;
};

struct buddy_embed_check {
    unsigned int can_fit;
    size_t offset;
    size_t buddy_size;
};

const unsigned int BUDDY_RELATIVE_MODE = 1;

struct buddy_tree;

struct buddy_tree_pos {
    size_t index;
    size_t depth;
};

struct buddy_tree_interval {
    struct buddy_tree_pos from;
    struct buddy_tree_pos to;
};

struct buddy_tree_walk_state {
    struct buddy_tree_pos starting_pos;
    struct buddy_tree_pos current_pos;
    unsigned int going_up;
    unsigned int walk_done;
};

enum buddy_tree_release_status {
    BUDDY_TREE_RELEASE_SUCCESS,
    BUDDY_TREE_RELEASE_FAIL_PARTIALLY_USED,
};

#define INVALID_POS buddy_tree_pos{ 0, 0 }

struct bitset_range {
    size_t from_bucket;
    size_t to_bucket;

    uint8_t from_index;
    uint8_t to_index;
};

/* Forward declarations (ignore) */
static unsigned int is_valid_alignment(size_t alignment);
static unsigned char *buddy_main(struct buddy *buddy);
static void buddy_toggle_virtual_slots(struct buddy *buddy, unsigned int state);
static struct buddy_embed_check buddy_embed_offset(size_t memory_size, size_t alignment);
static inline size_t size_for_depth(struct buddy *buddy, size_t depth);
static size_t depth_for_size(struct buddy *buddy, size_t requested_size);
static unsigned char *address_for_position(struct buddy *buddy, struct buddy_tree_pos pos);
static struct buddy_tree_pos position_for_address(struct buddy *buddy, const unsigned char *addr);
static size_t buddy_tree_sizeof(uint8_t order);
static size_t buddy_tree_order_for_memory(size_t memory_size, size_t alignment);
static struct buddy_tree *buddy_tree(struct buddy *buddy);
static struct buddy_tree *buddy_tree_init(unsigned char *at, uint8_t order);
static size_t buddy_tree_index(struct buddy_tree_pos pos);
static size_t buddy_tree_status(struct buddy_tree *t, struct buddy_tree_pos pos);
static inline size_t buddy_tree_depth(struct buddy_tree_pos pos);
static struct buddy_tree_pos buddy_tree_root(void);
static inline struct buddy_tree_pos buddy_tree_parent(struct buddy_tree_pos pos);
static inline struct buddy_tree_pos buddy_tree_right_child(struct buddy_tree_pos pos);
static inline struct buddy_tree_pos buddy_tree_left_child(struct buddy_tree_pos pos);
static struct buddy_tree_pos buddy_tree_leftmost_child(struct buddy_tree *t);
static struct buddy_tree_pos buddy_tree_find_free(struct buddy_tree *t, uint8_t target_depth);
static enum buddy_tree_release_status buddy_tree_release(struct buddy_tree *t, struct buddy_tree_pos pos);
static bool buddy_tree_valid(struct buddy_tree *t, struct buddy_tree_pos pos);
static void buddy_tree_mark(struct buddy_tree *t, struct buddy_tree_pos pos);
static size_t highest_bit_position(size_t value);
static inline size_t ceiling_power_of_two(size_t value);

size_t buddy_sizeof(size_t memory_size) {
    return buddy_sizeof_alignment(memory_size, BUDDY_ALLOC_ALIGN);
}

size_t buddy_sizeof_alignment(size_t memory_size, size_t alignment) {
    size_t buddy_tree_order;

    if (!is_valid_alignment(alignment)) {
        return 0; /* invalid */
    }
    if (memory_size < alignment) {
        return 0; /* invalid */
    }
    buddy_tree_order = buddy_tree_order_for_memory(memory_size, alignment);
    return sizeof(struct buddy) + buddy_tree_sizeof((uint8_t)buddy_tree_order);
}

struct buddy *buddy_init(unsigned char *at, unsigned char *main, size_t memory_size) {
    return buddy_init_alignment(at, main, memory_size, BUDDY_ALLOC_ALIGN);
}

struct buddy *buddy_init_alignment(unsigned char *at, unsigned char *main, size_t memory_size, size_t alignment) {
    size_t at_alignment, main_alignment, buddy_size, buddy_tree_order;
    struct buddy *buddy;

    if (at == NULL) {
        return NULL;
    }
    if (main == NULL) {
        return NULL;
    }
    if (at == main) {
        return NULL;
    }
    if (!is_valid_alignment(alignment)) {
        return NULL; /* invalid */
    }
    at_alignment = ((uintptr_t) at) % BUDDY_ALIGNOF(struct buddy);
    if (at_alignment != 0) {
        return NULL;
    }
    main_alignment = ((uintptr_t) main) % BUDDY_ALIGNOF(size_t);
    if (main_alignment != 0) {
        return NULL;
    }
    /* Trim down memory to alignment */
    if (memory_size % alignment) {
        memory_size -= (memory_size % alignment);
    }
    buddy_size = buddy_sizeof_alignment(memory_size, alignment);
    if (buddy_size == 0) {
        return NULL;
    }
    buddy_tree_order = buddy_tree_order_for_memory(memory_size, alignment);

    buddy = (struct buddy *) at;
    buddy->arena.main = main;
    buddy->memory_size = memory_size;
    buddy->buddy_flags = 0;
    buddy->alignment = alignment;
    buddy_tree_init((unsigned char *)buddy + sizeof(*buddy), (uint8_t) buddy_tree_order);
    buddy_toggle_virtual_slots(buddy, 1);
    return buddy;
}

struct buddy *buddy_embed(unsigned char *main, size_t memory_size) {
    return buddy_embed_alignment(main, memory_size, BUDDY_ALLOC_ALIGN);
}

struct buddy *buddy_embed_alignment(unsigned char *main, size_t memory_size, size_t alignment) {
    struct buddy_embed_check check_result;
    struct buddy *buddy;

    if (! main) {
        return NULL;
    }
    if (!is_valid_alignment(alignment)) {
        return NULL; /* invalid */
    }
    check_result = buddy_embed_offset(memory_size, alignment);
    if (! check_result.can_fit) {
        return NULL;
    }

    buddy = buddy_init_alignment(main+check_result.offset, main, check_result.offset, alignment);
    if (! buddy) { /* regular initialization failed */
        return NULL;
    }

    buddy->buddy_flags |= BUDDY_RELATIVE_MODE;
    buddy->arena.main_offset = (unsigned char *)buddy - main;
    return buddy;
}

void *buddy_malloc(struct buddy *buddy, size_t requested_size) {
    size_t target_depth;
    struct buddy_tree *tree;
    struct buddy_tree_pos pos;

    if (buddy == NULL) {
        return NULL;
    }
    if (requested_size == 0) {
        /*
         * See https://wiki.sei.cmu.edu/confluence/display/c/MEM04-C.+Beware+of+zero-length+allocations
         */
        requested_size = 1;
    }
    if (requested_size > buddy->memory_size) {
        return NULL;
    }

    target_depth = depth_for_size(buddy, requested_size);
    BUDDY_PRINTF("buddy_malloc: requested size %zu, target depth %zu\n",
        requested_size, target_depth);
    tree = buddy_tree(buddy);

    /* O(log(n)) traversal through the tree */
    pos = buddy_tree_find_free(tree, (uint8_t) target_depth);

    if (! buddy_tree_valid(tree, pos)) {
        return NULL; /* no slot found */
    }

    /* Allocate the slot */
    buddy_tree_mark(tree, pos);

    /* Find and return the actual memory address */
    return address_for_position(buddy, pos);
}

void buddy_dealloc(struct buddy *buddy, void *ptr) {
    unsigned char *dst, *main;
    struct buddy_tree *tree;
    struct buddy_tree_pos pos;

    if (buddy == NULL) {
        return;
    }
    if (ptr == NULL) {
        return;
    }
    dst = (unsigned char *)ptr;
    main = buddy_main(buddy);
    if ((dst < main) || (dst >= (main + buddy->memory_size))) {
        return;
    }

    /* Find the position tracking this address */
    tree = buddy_tree(buddy);
    pos = position_for_address(buddy, dst);

    if (! buddy_tree_valid(tree, pos)) {
        return;
    }

    /* Release the position */
    buddy_tree_release(tree, pos);
}

void *buddy_realloc(struct buddy *buddy, void *ptr, size_t requested_size) {
    struct buddy_tree *tree;
    struct buddy_tree_pos origin, new_pos;
    size_t current_depth, target_depth;
    void *source, *destination;

    /*
     * - NULL ptr degrades into malloc
     * - Zero size degrades into free
     * - Same size as previous malloc is a no-op or a rellocation
     * - Smaller size than previous *alloc decrease the allocated size with an optional rellocation
     * - If the new allocation cannot be satisfied NULL is returned BUT the slot is preserved
     * - Larger size than previous *alloc increase tha allocated size with an optional rellocation
     * - unlike the C realloc, this reallocator does not copy data from the previous block to the 
     *   new block as for the intended use it's unnecessary
     */
    if (ptr == NULL) {
        return buddy_malloc(buddy, requested_size);
    }
    if (requested_size == 0) {
        buddy_dealloc(buddy, ptr);
        return NULL;
    }
    if (requested_size > buddy->memory_size) {
        return NULL;
    }

    /* Find the position tracking this address */
    tree = buddy_tree(buddy);
    origin = position_for_address(buddy, (unsigned char *) ptr);
    if (! buddy_tree_valid(tree, origin)) {
        return NULL;
    }
    current_depth = buddy_tree_depth(origin);
    target_depth = depth_for_size(buddy, requested_size);

    /* Release the position and perform a search */
    buddy_tree_release(tree, origin);
    new_pos = buddy_tree_find_free(tree, (uint8_t) target_depth);

    if (! buddy_tree_valid(tree, new_pos)) {
        /* allocation failure, restore mark and return null */
        buddy_tree_mark(tree, origin);
        return NULL;
    }

    if (origin.index == new_pos.index) {
        /* Allocated to the same slot, restore mark and return null */
        buddy_tree_mark(tree, origin);
        return ptr;
    }

    destination = address_for_position(buddy, new_pos);

    /* Allocate and return */
    buddy_tree_mark(tree, new_pos);
    return destination;
}

static unsigned int is_valid_alignment(size_t alignment) {
    return ceiling_power_of_two(alignment) == alignment;
}

static size_t buddy_effective_memory_size(struct buddy *buddy) {
    return ceiling_power_of_two(buddy->memory_size);
}

static size_t buddy_tree_order_for_memory(size_t memory_size, size_t alignment) {
    size_t blocks = memory_size / alignment;
    return highest_bit_position(ceiling_power_of_two(blocks));
}

static size_t depth_for_size(struct buddy *buddy, size_t requested_size) {
    size_t depth, effective_memory_size;
    if (requested_size < buddy->alignment) {
        requested_size = buddy->alignment;
    }
    depth = 1;
    effective_memory_size = buddy_effective_memory_size(buddy);
    while ((effective_memory_size / requested_size) >> 1u) {
        depth++;
        effective_memory_size >>= 1u;
    }
    return depth;
}

static inline size_t size_for_depth(struct buddy *buddy, size_t depth) {
    return ceiling_power_of_two(buddy->memory_size) >> (depth-1);
}

static struct buddy_tree *buddy_tree(struct buddy *buddy) {
    return (struct buddy_tree*) ((unsigned char *)buddy + sizeof(*buddy));
}

static size_t buddy_virtual_slots(struct buddy *buddy) {
    size_t memory_size = buddy->memory_size;
    size_t effective_memory_size = buddy_effective_memory_size(buddy);
    if (effective_memory_size == memory_size) {
        return 0;
    }
    return (effective_memory_size - memory_size) / buddy->alignment;
}

static unsigned char *address_for_position(struct buddy *buddy, struct buddy_tree_pos pos) {
    size_t block_size = size_for_depth(buddy, buddy_tree_depth(pos));
    size_t addr = block_size * buddy_tree_index(pos);
    return buddy_main(buddy) + addr;
}

static struct buddy_tree_pos deepest_position_for_offset(struct buddy *buddy, size_t offset) {
    size_t index = offset / buddy->alignment;
    struct buddy_tree_pos pos = buddy_tree_leftmost_child(buddy_tree(buddy));
    pos.index += index;
    return pos;
}

static struct buddy_tree_pos position_for_address(struct buddy *buddy, const unsigned char *addr) {
    unsigned char *main;
    struct buddy_tree *tree;
    struct buddy_tree_pos pos;
    size_t offset;

    main = buddy_main(buddy);
    offset = (size_t) (addr - main);

    if (offset % buddy->alignment) {
        return INVALID_POS; /* invalid alignment */
    }

    tree = buddy_tree(buddy);
    pos = deepest_position_for_offset(buddy, offset);

    /* Find the actual allocated position tracking this address */
    while (!buddy_tree_status(tree, pos)) {
        pos = buddy_tree_parent(pos);

        if (!buddy_tree_valid(tree, pos)) {
            return INVALID_POS;
        }
    }

    if (address_for_position(buddy, pos) != addr) {
        return INVALID_POS; /* invalid alignment */
    }

    return pos;
}

static unsigned int buddy_relative_mode(struct buddy *buddy) {
    return (unsigned int)buddy->buddy_flags & BUDDY_RELATIVE_MODE;
}

static unsigned char *buddy_main(struct buddy *buddy) {
    if (buddy_relative_mode(buddy)) {
        return (unsigned char *)buddy - buddy->arena.main_offset;
    }
    return buddy->arena.main;
}

static void buddy_toggle_virtual_slots(struct buddy *buddy, unsigned int state) {
    size_t delta, memory_size, effective_memory_size;
    struct buddy_tree *tree;
    struct buddy_tree_pos pos;

    memory_size = buddy->memory_size;
    /* Mask/unmask the virtual space if memory is not a power of two */
    effective_memory_size = buddy_effective_memory_size(buddy);
    if (effective_memory_size == memory_size) {
        return;
    }

    /* Get the area that we need to mask and pad it to alignment */
    /* Node memory size is already aligned to buddy->alignment */
    delta = effective_memory_size - memory_size;

    tree = buddy_tree(buddy);
    pos = buddy_tree_right_child(buddy_tree_root());
    while (delta) {
        size_t current_pos_size = size_for_depth(buddy, buddy_tree_depth(pos));
        if (delta == current_pos_size) {
            /* toggle current pos */
            if (state) {
                buddy_tree_mark(tree, pos);
            }
            else {
                buddy_tree_release(tree, pos);
            }
            break;
        }
        if (delta <= (current_pos_size / 2)) {
            /* re-run for right child */
            pos = buddy_tree_right_child(pos);
            continue;
        } else {
            /* toggle right child */
            if (state) {
                buddy_tree_mark(tree, buddy_tree_right_child(pos));
            }
            else {
                buddy_tree_release(tree, buddy_tree_right_child(pos));
            }
            /* reduce delta */
            delta -= current_pos_size / 2;
            /* re-run for left child */
            pos = buddy_tree_left_child(pos);
            continue;
        }
    }
}

static struct buddy_embed_check buddy_embed_offset(size_t memory_size, size_t alignment) {
    size_t buddy_size, offset;
    struct buddy_embed_check check_result;

    memset(&check_result, 0, sizeof(check_result));
    check_result.can_fit = 1;
    buddy_size = buddy_sizeof_alignment(memory_size, alignment);
    if (buddy_size >= memory_size) {
        check_result.can_fit = 0;
    }

    offset = memory_size - buddy_size;
    if (offset % BUDDY_ALIGNOF(struct buddy) != 0) {
        buddy_size += offset % BUDDY_ALIGNOF(struct buddy);
        if (buddy_size >= memory_size) {
            check_result.can_fit = 0;
        }
        offset = memory_size - buddy_size;
    }

    if (check_result.can_fit) {
        check_result.offset = offset;
        check_result.buddy_size = buddy_size;
    }
    return check_result;
}

/*
 * A buddy allocation tree
 */

struct buddy_tree {
    size_t upper_pos_bound;
    size_t size_for_order_offset;
    uint8_t order;
    uint8_t flags;
};

struct internal_position {
    size_t local_offset;
    size_t bitset_location;
};

static inline size_t size_for_order(uint8_t order, uint8_t to);
static inline size_t buddy_tree_index_internal(struct buddy_tree_pos pos);
static struct buddy_tree_pos buddy_tree_leftmost_child_internal(size_t tree_order);
static struct internal_position buddy_tree_internal_position_order(size_t tree_order, struct buddy_tree_pos pos);
static struct internal_position buddy_tree_internal_position_tree(struct buddy_tree *t, struct buddy_tree_pos pos);
static void update_parent_chain(struct buddy_tree *t, struct buddy_tree_pos pos,struct internal_position pos_internal, size_t size_current);
static inline unsigned char *buddy_tree_bits(struct buddy_tree *t);
static void buddy_tree_populate_size_for_order(struct buddy_tree *t);
static inline size_t buddy_tree_size_for_order(struct buddy_tree *t, uint8_t to);
static void write_to_internal_position(struct buddy_tree* t, struct internal_position pos, size_t value);
static size_t read_from_internal_position(unsigned char *bitset, struct internal_position pos);
static inline unsigned char compare_with_internal_position(unsigned char *bitset, struct internal_position pos, size_t value);

static inline size_t size_for_order(uint8_t order, uint8_t to) {
    size_t result = 0;
    size_t multi = 1u;
    while (order != to) {
        result += order * multi;
        order--;
        multi *= 2;
    }
    return result;
}

static inline struct internal_position buddy_tree_internal_position_order(
        size_t tree_order, struct buddy_tree_pos pos) {
    struct internal_position p;
    size_t total_offset, local_index;

    p.local_offset = tree_order - buddy_tree_depth(pos) + 1;
    total_offset = size_for_order((uint8_t) tree_order, (uint8_t) p.local_offset);
    local_index = buddy_tree_index_internal(pos);
    p.bitset_location = total_offset + (p.local_offset * local_index);
    return p;
}

static inline struct internal_position buddy_tree_internal_position_tree(
        struct buddy_tree *t, struct buddy_tree_pos pos) {
    struct internal_position p;
    size_t total_offset, local_index;

    p.local_offset = t->order - buddy_tree_depth(pos) + 1;
    total_offset = buddy_tree_size_for_order(t, (uint8_t) p.local_offset);
    local_index = buddy_tree_index_internal(pos);
    p.bitset_location = total_offset + (p.local_offset * local_index);
    return p;
}

static size_t buddy_tree_sizeof(uint8_t order) {
    size_t tree_size, bitset_size, size_for_order_size;

    tree_size = sizeof(struct buddy_tree);
    /* Account for the bitset */
    bitset_size = bitset_sizeof(size_for_order(order, 0));
    if (bitset_size % sizeof(size_t)) {
        bitset_size += (bitset_size % sizeof(size_t));
    }
    /* Account for the size_for_order memoization */
    size_for_order_size = ((order+2) * sizeof(size_t));
    return tree_size + bitset_size + size_for_order_size;
}

static struct buddy_tree *buddy_tree_init(unsigned char *at, uint8_t order) {
    size_t size = buddy_tree_sizeof(order);
    struct buddy_tree *t = (struct buddy_tree*) at;
    memset(at, 0, size);
    t->order = order;
    t->upper_pos_bound = two_to_the_power_of(t->order);
    buddy_tree_populate_size_for_order(t);
    return t;
}

static bool buddy_tree_valid(struct buddy_tree *t, struct buddy_tree_pos pos) {
    return pos.index && (pos.index < t->upper_pos_bound);
}

static uint8_t buddy_tree_order(struct buddy_tree *t) {
    return t->order;
}

static struct buddy_tree_pos buddy_tree_root(void) {
    struct buddy_tree_pos identity = { 1, 1 };
    return identity;
}

static struct buddy_tree_pos buddy_tree_leftmost_child(struct buddy_tree *t) {
    return buddy_tree_leftmost_child_internal(t->order);
}

static struct buddy_tree_pos buddy_tree_leftmost_child_internal(size_t tree_order) {
    struct buddy_tree_pos result;
    result.index = two_to_the_power_of(tree_order - 1u);
    result.depth = tree_order;
    return result;
}

static inline size_t buddy_tree_depth(struct buddy_tree_pos pos) {
    return pos.depth;
}

static inline struct buddy_tree_pos buddy_tree_left_child(struct buddy_tree_pos pos) {
    pos.index *= 2;
    pos.depth++;
    return pos;
}

static inline struct buddy_tree_pos buddy_tree_right_child(struct buddy_tree_pos pos) {
    pos.index *= 2;
    pos.index++;
    pos.depth++;
    return pos;
}

static inline struct buddy_tree_pos buddy_tree_sibling(struct buddy_tree_pos pos) {
    pos.index ^= 1;
    return pos;
}

static inline struct buddy_tree_pos buddy_tree_parent(struct buddy_tree_pos pos) {
    pos.index /= 2;
    pos.depth--;
    return pos;
}

static struct buddy_tree_pos buddy_tree_right_adjacent(struct buddy_tree_pos pos) {
    if (((pos.index + 1) ^ pos.index) > pos.index) {
        return INVALID_POS;
    }
    pos.index++;
    return pos;
}

static size_t buddy_tree_index(struct buddy_tree_pos pos) {
    return buddy_tree_index_internal(pos);
}

static inline size_t buddy_tree_index_internal(struct buddy_tree_pos pos) {
    /* Clear out the highest bit, this gives us the index
     * in a row of sibling nodes */
    size_t mask = two_to_the_power_of(pos.depth - 1u);
    size_t result = pos.index & ~mask;
    return result;
}

static inline unsigned char *buddy_tree_bits(struct buddy_tree *t) {
    return ((unsigned char *) t) + sizeof(*t);
}

static void buddy_tree_populate_size_for_order(struct buddy_tree *t) {
    size_t bitset_offset = bitset_sizeof(size_for_order(t->order, 0));
    if (bitset_offset % sizeof(size_t)) {
        bitset_offset += (bitset_offset % sizeof(size_t));
    }
    t->size_for_order_offset = bitset_offset / sizeof(size_t);
    t->size_for_order_offset++;
    for (size_t i = 0; i <= t->order; i++) {
        *((size_t *)(((unsigned char *) t) + sizeof(*t)) + t->size_for_order_offset + i) = size_for_order(t->order, (uint8_t) i);
    }
}

static inline size_t buddy_tree_size_for_order(struct buddy_tree *t,
         uint8_t to) {
    return *((size_t *)(((unsigned char *) t) + sizeof(*t)) + t->size_for_order_offset + to);
}

static void write_to_internal_position(struct buddy_tree* t, struct internal_position pos, size_t value) {
    unsigned char *bitset = buddy_tree_bits(t);
    struct bitset_range clear_range = bitset_range(pos.bitset_location, pos.bitset_location + pos.local_offset - 1);

    bitset_clear_range(bitset, clear_range);
    if (value) {
        bitset_set_range(bitset, bitset_range(pos.bitset_location, pos.bitset_location+value-1));
    }
}

static size_t read_from_internal_position(unsigned char *bitset, struct internal_position pos) {
    if (! bitset_test(bitset, pos.bitset_location)) {
        return 0; /* Fast test without complete extraction */
    }
    return bitset_count_range(bitset, bitset_range(pos.bitset_location, pos.bitset_location+pos.local_offset-1));
}

static inline unsigned char compare_with_internal_position(unsigned char *bitset, struct internal_position pos, size_t value) {
    return bitset_test(bitset, pos.bitset_location+value-1);
}

static struct buddy_tree_interval buddy_tree_interval(struct buddy_tree *t, struct buddy_tree_pos pos) {
    struct buddy_tree_interval result;
    size_t depth;

    result.from = pos;
    result.to = pos;
    depth = pos.depth;
    while (depth != t->order) {
        result.from = buddy_tree_left_child(result.from);
        result.to = buddy_tree_right_child(result.to);
        depth += 1;
    }
    return result;
}

static bool buddy_tree_interval_contains(struct buddy_tree_interval outer,
        struct buddy_tree_interval inner) {
    return (inner.from.index >= outer.from.index)
        && (inner.from.index <= outer.to.index)
        && (inner.to.index >= outer.from.index)
        && (inner.to.index <= outer.to.index);
}

static struct buddy_tree_walk_state buddy_tree_walk_state_root(void) {
    struct buddy_tree_walk_state state;
    memset(&state, 0, sizeof(state));
    state.starting_pos = buddy_tree_root();
    state.current_pos = buddy_tree_root();
    return state;
}

static unsigned int buddy_tree_walk(struct buddy_tree *t, struct buddy_tree_walk_state *state) {
    do {
        if (state->going_up) {
            if (state->current_pos.index == state->starting_pos.index) {
                state->walk_done = 1;
                state->going_up = 0;
            } else if (state->current_pos.index & 1u) {
                state->current_pos = buddy_tree_parent(state->current_pos); /* Ascend */
            } else {
                state->current_pos = buddy_tree_right_adjacent(state->current_pos); /* Descend right */
                state->going_up = 0;
            }
        } else if (buddy_tree_valid(t, buddy_tree_left_child(state->current_pos))) {
            /* Descend left */
            state->current_pos = buddy_tree_left_child(state->current_pos);
        } else { /* Ascend */
            state->going_up = 1;
        }
    } while(state->going_up);
    return ! state->walk_done;
}

static size_t buddy_tree_status(struct buddy_tree *t, struct buddy_tree_pos pos) {
    struct internal_position internal = buddy_tree_internal_position_tree(t, pos);
    return read_from_internal_position(buddy_tree_bits(t), internal);
}

static void buddy_tree_mark(struct buddy_tree *t, struct buddy_tree_pos pos) {
    /* Calling mark on a used position is a bug in caller */
    struct internal_position internal = buddy_tree_internal_position_tree(t, pos);

    /* Mark the node as used */
    write_to_internal_position(t, internal, internal.local_offset);

    /* Update the tree upwards */
    update_parent_chain(t, pos, internal, internal.local_offset);
}

static enum buddy_tree_release_status buddy_tree_release(struct buddy_tree *t, struct buddy_tree_pos pos) {
    /* Calling release on an unused or a partially-used position a bug in caller */
    struct internal_position internal = buddy_tree_internal_position_tree(t, pos);

    if (read_from_internal_position(buddy_tree_bits(t), internal) != internal.local_offset) {
        return BUDDY_TREE_RELEASE_FAIL_PARTIALLY_USED;
    }

    /* Mark the node as unused */
    write_to_internal_position(t, internal, 0);

    /* Update the tree upwards */
    update_parent_chain(t, pos, internal, 0);

    return BUDDY_TREE_RELEASE_SUCCESS;
}

static void update_parent_chain(struct buddy_tree *t, struct buddy_tree_pos pos,
        struct internal_position pos_internal, size_t size_current) {
    size_t size_sibling, size_parent, target_parent;
    unsigned char *bits = buddy_tree_bits(t);

    while (pos.index != 1) {
        pos_internal.bitset_location += pos_internal.local_offset
            - (2 * pos_internal.local_offset * (pos.index & 1u));
        size_sibling = read_from_internal_position(bits, pos_internal);

        pos = buddy_tree_parent(pos);
        pos_internal = buddy_tree_internal_position_tree(t, pos);
        size_parent = read_from_internal_position(bits, pos_internal);

        target_parent = (size_current || size_sibling)
            * ((size_current <= size_sibling ? size_current : size_sibling) + 1);
        if (target_parent == size_parent) {
            return;
        }

        write_to_internal_position(t, pos_internal, target_parent);
        size_current = target_parent;
    };
}

static struct buddy_tree_pos buddy_tree_find_free(struct buddy_tree *t, uint8_t target_depth) {
    struct buddy_tree_pos current_pos, left_pos, right_pos;
    uint8_t target_status;
    size_t current_depth, right_status;
    struct internal_position left_internal, right_internal;
    unsigned char *tree_bits;

    current_pos = buddy_tree_root();
    target_status = target_depth - 1;
    current_depth = buddy_tree_depth(current_pos);
    if (buddy_tree_status(t, current_pos) > target_status) {
        return INVALID_POS; /* No position available down the tree */
    }
    tree_bits = buddy_tree_bits(t);
    while (current_depth != target_depth) {
        /* Advance criteria */
        target_status -= 1;
        current_depth += 1;

        left_pos = buddy_tree_left_child(current_pos);
        right_pos = buddy_tree_sibling(left_pos);

        left_internal = buddy_tree_internal_position_tree(t, left_pos);

        right_internal = left_internal;
        right_internal.bitset_location += right_internal.local_offset; /* advance to the right */

        if (compare_with_internal_position(tree_bits, left_internal, target_status+1)) { /* left branch is busy, pick right */
            current_pos = right_pos;
        } else if (compare_with_internal_position(tree_bits, right_internal, target_status+1)) { /* right branch is busy, pick left */
            current_pos = left_pos;
        } else {
            /* One of the child nodes must be read in order to compare it to its sibling. */
            right_status = read_from_internal_position(tree_bits, right_internal);
            if (right_status) {
                if (compare_with_internal_position(tree_bits, left_internal, right_status)) {
                    current_pos = left_pos; /* Left is equal or more busy than right, prefer left */
                } else {
                    current_pos = right_pos;
                }
            } else { /* Right is empty, prefer left */
                current_pos = left_pos;
            }
        }
    }
    return current_pos;
}

static bool buddy_tree_is_free(struct buddy_tree *t, struct buddy_tree_pos pos) {
    if (buddy_tree_status(t, pos)) {
        return false;
    }
    pos = buddy_tree_parent(pos);
    while(buddy_tree_valid(t, pos)) {
        struct internal_position internal = buddy_tree_internal_position_tree(t, pos);
        size_t value = read_from_internal_position(buddy_tree_bits(t), internal);
        if (value) {
            return value != internal.local_offset;
        }
        pos = buddy_tree_parent(pos);
    }
    return true;
}

static void buddy_tree_debug(struct buddy_tree *t, struct buddy_tree_pos pos,
        size_t start_size) {
    struct buddy_tree_walk_state state = buddy_tree_walk_state_root();
    state.current_pos = pos;
    do {
        struct internal_position pos_internal = buddy_tree_internal_position_tree(t, state.current_pos);
        size_t pos_status = read_from_internal_position(buddy_tree_bits(t), pos_internal);
        size_t pos_size = start_size >> ((buddy_tree_depth(state.current_pos) - 1u) % ((sizeof(size_t) * CHAR_BIT)-1));
        BUDDY_PRINTF("%.*s",
            (int) buddy_tree_depth(state.current_pos),
            "                                                               ");
        BUDDY_PRINTF("pos index: %zu pos depth: %zu status: %zu bitset-len: %zu bitset-at: %zu",
            state.current_pos.index, state.current_pos.depth, pos_status,
            pos_internal.local_offset, pos_internal.bitset_location);
        if (pos_status == pos_internal.local_offset) {
            BUDDY_PRINTF(" size: %zu", pos_size);
        }
        BUDDY_PRINTF("\n");
    } while (buddy_tree_walk(t, &state));
}

unsigned int buddy_tree_check_invariant(struct buddy_tree *t, struct buddy_tree_pos pos) {
    unsigned int fail = 0;
    struct buddy_tree_walk_state state = buddy_tree_walk_state_root();
    state.current_pos = pos;
    do {
        struct internal_position current_internal = buddy_tree_internal_position_tree(t, pos);
        size_t current_status = read_from_internal_position(buddy_tree_bits(t), current_internal);
        size_t left_child_status = buddy_tree_status(t, buddy_tree_left_child(pos));
        size_t right_child_status = buddy_tree_status(t, buddy_tree_right_child(pos));
        unsigned int violated = 0;

        if (left_child_status || right_child_status) {
            size_t min = left_child_status <= right_child_status
                ? left_child_status : right_child_status;
            if (current_status != (min + 1)) {
                violated = 1;
            }
        } else {
            if ((current_status > 0) && (current_status < current_internal.local_offset)) {
                violated = 1;
            }
        }

        if (violated) {
            fail = 1;
            BUDDY_PRINTF("invariant violation at position [ index: %zu depth: %zu ]!\n", pos.index, pos.depth);
            BUDDY_PRINTF("current: %zu left %zu right %zu max %zu\n",
                current_status, left_child_status, right_child_status, current_internal.local_offset);
        }

    } while (buddy_tree_walk(t, &state));
    return fail;
}

/*
 * Calculate tree fragmentation based on free slots.
 * Based on https://asawicki.info/news_1757_a_metric_for_memory_fragmentation
 */
static unsigned char buddy_tree_fragmentation(struct buddy_tree *t) {
    const unsigned char fractional_bits = 8;
    const unsigned char fractional_mask = 255;

    uint8_t tree_order;
    size_t root_status, quality, total_free_size, virtual_size, quality_percent;
    struct buddy_tree_walk_state state;

    tree_order = buddy_tree_order(t);
    root_status = buddy_tree_status(t, buddy_tree_root());
    if (root_status == 0) { /* Emptry tree */
        return 0;
    }

    quality = 0;
    total_free_size = 0;

    state = buddy_tree_walk_state_root();
    do {
        size_t pos_status = buddy_tree_status(t, state.current_pos);
        if (pos_status == 0) {
            /* Empty node, process */
            virtual_size = two_to_the_power_of((tree_order - state.current_pos.depth) % ((sizeof(size_t) * CHAR_BIT)-1));
            quality += (virtual_size * virtual_size);
            total_free_size += virtual_size;
            /* Ascend */
            state.going_up = 1;
        } else if (pos_status == (tree_order - state.current_pos.depth + 1)) {
            /* Busy node, ascend */
            state.going_up = 1;
        }
    } while (buddy_tree_walk(t, &state));

    if (total_free_size == 0) { /* Fully-allocated tree */
        return 0;
    }

    quality_percent = (integer_square_root(quality) << fractional_bits) / total_free_size;
    quality_percent *= quality_percent;
    quality_percent >>= fractional_bits;
    return fractional_mask - (quality_percent & fractional_mask);
}

/*
 * A char-backed bitset implementation
 */

/* returns number of bytes needed to store a bitset of "elements" bits*/
size_t bitset_sizeof(size_t elements) {
    return ((elements) + CHAR_BIT - 1u) / CHAR_BIT;
}

static uint8_t bitset_index_mask[8] = {1, 2, 4, 8, 16, 32, 64, 128};

static inline void bitset_set(unsigned char *bitset, size_t pos) {
    size_t bucket = pos / CHAR_BIT;
    size_t index = pos % CHAR_BIT;
    bitset[bucket] |= bitset_index_mask[index];
}

static inline void bitset_clear(unsigned char *bitset, size_t pos) {
    size_t bucket = pos / CHAR_BIT;
    size_t index = pos % CHAR_BIT;
    bitset[bucket] &= ~bitset_index_mask[index];
}

static inline bool bitset_test(const unsigned char *bitset, size_t pos) {
    size_t bucket = pos / CHAR_BIT;
    size_t index = pos % CHAR_BIT;
    return bitset[bucket] & bitset_index_mask[index];
}

static const uint8_t bitset_char_mask[8][8] = {
    {1, 3, 7, 15, 31, 63, 127, 255},
    {0, 2, 6, 14, 30, 62, 126, 254},
    {0, 0, 4, 12, 28, 60, 124, 252},
    {0, 0, 0,  8, 24, 56, 120, 248},
    {0, 0, 0,  0, 16, 48, 112, 240},
    {0, 0, 0,  0,  0, 32,  96, 224},
    {0, 0, 0,  0,  0,  0,  64, 192},
    {0, 0, 0,  0,  0,  0,   0, 128},
};

static inline struct bitset_range bitset_range(size_t from_pos, size_t to_pos) {
    struct bitset_range range = {0};
    range.from_bucket = from_pos / CHAR_BIT;
    range.to_bucket = to_pos / CHAR_BIT;

    range.from_index = from_pos % CHAR_BIT;
    range.to_index = to_pos % CHAR_BIT;
    return range;
}

static void bitset_set_range(unsigned char *bitset, struct bitset_range range) {
    if (range.from_bucket == range.to_bucket) {
        bitset[range.from_bucket] |= bitset_char_mask[range.from_index][range.to_index];
    } else {
        bitset[range.from_bucket] |= bitset_char_mask[range.from_index][7];
        bitset[range.to_bucket] |= bitset_char_mask[0][range.to_index];
        while(++range.from_bucket != range.to_bucket) {
            bitset[range.from_bucket] = 255u;
        }
    }
}

static void bitset_clear_range(unsigned char* bitset, struct bitset_range range) {
    if (range.from_bucket == range.to_bucket) {
        bitset[range.from_bucket] &= ~bitset_char_mask[range.from_index][range.to_index];
    }
    else {
        bitset[range.from_bucket] &= ~bitset_char_mask[range.from_index][7];
        bitset[range.to_bucket] &= ~bitset_char_mask[0][range.to_index];
        while (++range.from_bucket != range.to_bucket) {
            bitset[range.from_bucket] = 0;
        }
    }
}

static size_t bitset_count_range(unsigned char *bitset, struct bitset_range range) {
    size_t result;

    if (range.from_bucket == range.to_bucket) {
        return popcount_byte(bitset[range.from_bucket] & bitset_char_mask[range.from_index][range.to_index]);
    }

    result = popcount_byte(bitset[range.from_bucket] & bitset_char_mask[range.from_index][7])
        + popcount_byte(bitset[range.to_bucket]  & bitset_char_mask[0][range.to_index]);
    while(++range.from_bucket != range.to_bucket) {
        result += popcount_byte(bitset[range.from_bucket]);
    }
    return result;
}

static void bitset_shift_left(unsigned char *bitset, size_t from_pos, size_t to_pos, size_t by) {
    size_t length = to_pos - from_pos;
    for(size_t i = 0; i < length; i++) {
        size_t at = from_pos + i;
        if (bitset_test(bitset, at)) {
            bitset_set(bitset, at-by);
        } else {
            bitset_clear(bitset, at-by);
        }
        bitset_clear(bitset, at);
    }
}

static void bitset_shift_right(unsigned char *bitset, size_t from_pos, size_t to_pos, size_t by) {
    ssize_t length = (ssize_t) to_pos - (ssize_t) from_pos;
    while (length >= 0) {
        size_t at = from_pos + (size_t) length;
        if (bitset_test(bitset, at)) {
            bitset_set(bitset, at+by);
        } else {
            bitset_clear(bitset, at+by);
        }
        bitset_clear(bitset, at);
        length -= 1;
    }
}

void bitset_debug(unsigned char *bitset, size_t length) {
    for (size_t i = 0; i < length; i++) {
        BUDDY_PRINTF("%zu: %d\n", i, bitset_test(bitset, i) > 0);
    }
}

/*
 Bits
*/

static const unsigned char popcount_lookup[256] = {
    0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8
};

static inline unsigned int popcount_byte(unsigned char b) {
    return popcount_lookup[b];
}

/* Returns the highest set bit position for the given value. Returns zero for zero. */
static size_t highest_bit_position(size_t value) {
    size_t result = 0;
    /* some other millennia when size_t becomes 128-bit this will break :) */
    const size_t all_set[] = {65535, 255, 15, 7, 3, 1};
    const size_t count[] = {16, 8, 4, 2, 1, 1};

    for (size_t i = 0; i < (sizeof all_set / sizeof *all_set); i++) {
        if (value >= all_set[i]) {
            value >>= count[i];
            result += count[i];
        }
    }
    return result + value;
}

static inline size_t ceiling_power_of_two(size_t value) {
    value += !value; /* branchless x -> { 1 for 0, x for x } */
    return two_to_the_power_of(highest_bit_position(value + value - 1)-1);
}

static inline size_t two_to_the_power_of(size_t order) {
    return ((size_t)1) << order;
}

static inline size_t integer_square_root(size_t op) {
    /* by Martin Guy, 1985 - http://medialab.freaknet.org/martin/src/sqrt/ */
    size_t result = 0;
    size_t cursor = (SIZE_MAX - (SIZE_MAX >> 1)) >> 1; /* second-to-top bit set */
    while (cursor > op) {
        cursor >>= 2;
    }
    /* "cursor" starts at the highest power of four <= than the argument. */
    while (cursor != 0) {
        if (op >= result + cursor) {
            op -= result + cursor;
            result += 2 * cursor;
        }
        result >>= 1;
        cursor >>= 2;
    }
    return result;
}

static void _print_subtree(struct buddy *buddy,
                           struct buddy_tree_pos pos,
                           const char *prefix,
                           bool is_last)
{
    struct buddy_tree *t = buddy_tree(buddy);
    bool isFree = buddy_tree_is_free(t, pos);
    size_t sz = size_for_depth(buddy, buddy_tree_depth(pos));
    void *addr = address_for_position(buddy, pos);

    // Print in the requested format:
    //   └── size: 32, address: 0x5ccb73b0, occupied: true
    printf("%s%s size: %zu, address: %p, occupied: %s\n",
           prefix,
           is_last ? "└──" : "├──",
           sz,
           addr,
           isFree ? "false" : "true");

    // Build prefix for children
    char child_prefix[256];
    strcpy(child_prefix, prefix);
    strcat(child_prefix, is_last ? "    " : "│   ");

    // Recurse left then right
    struct buddy_tree_pos L = buddy_tree_left_child(pos);
    struct buddy_tree_pos R = buddy_tree_right_child(pos);
    bool has_left  = buddy_tree_valid(t, L);
    bool has_right = buddy_tree_valid(t, R);

    if (has_left)
        _print_subtree(buddy, L, child_prefix, !has_right);
    if (has_right)
        _print_subtree(buddy, R, child_prefix, true);
}

void print_buddy_tree(struct buddy *buddy)
{
    struct buddy_tree_pos root = buddy_tree_root();                  /* :contentReference[oaicite:12]{index=12}:contentReference[oaicite:13]{index=13} */
    _print_subtree(buddy, root, "", 1);
}

void buddy_debug(struct buddy *buddy) {
    BUDDY_PRINTF("buddy allocator at: %p arena at: %p\n", (void *)buddy, (void *)buddy_main(buddy));
    BUDDY_PRINTF("memory size: %zu\n", buddy->memory_size);
    BUDDY_PRINTF("mode: ");
    if (buddy_relative_mode(buddy)) {
        BUDDY_PRINTF("embedded");
    } else {
        BUDDY_PRINTF("standard");
    }
    BUDDY_PRINTF("\n");
    BUDDY_PRINTF("virtual slots: %zu\n", buddy_virtual_slots(buddy));
    BUDDY_PRINTF("allocator tree follows:\n");
    print_buddy_tree(buddy);
}