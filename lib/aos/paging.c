/**
 * \file
 * \brief AOS paging helpers.
 */

/*
 * Copyright (c) 2012, 2013, 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <aos/aos.h>
#include <aos/paging.h>
#include <aos/except.h>
#include <aos/slab.h>
#include "threads_priv.h"

#include <stdio.h>
#include <string.h>

static struct paging_state current;

static char heap[PAGING_HEAP_SIZE] = { 0 };
static char* currp = heap;
static char* endp = heap + PAGING_HEAP_SIZE;

static inline char* paging_heap_malloc(size_t size)
{
    size = ROUND_UP(size, 4);
    char* buf = currp;
    currp += size;
    if (currp > endp) {
        return NULL;
    }
    return buf;
}

/**
 * \brief Helper function that allocates a slot and
 *        creates a ARM l2 page table capability
 */
__attribute__((unused))
static errval_t arml2_alloc(struct paging_state * st, struct capref *ret)
{
    errval_t err;
    err = st->slot_alloc->alloc(st->slot_alloc, ret);
    if (err_is_fail(err)) {
        debug_printf("slot_alloc failed: %s\n", err_getstring(err));
        return err;
    }
    err = vnode_create(*ret, ObjType_VNode_ARM_l2);
    if (err_is_fail(err)) {
        debug_printf("vnode_create failed: %s\n", err_getstring(err));
        return err;
    }
    return SYS_ERR_OK;
}

errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir, struct slot_allocator *ca)
{
    debug_printf("paging_init_state %p\n", st);

    st->mapping_cb = NULL;

    // M2:
    // Slot allocator.
    st->slot_alloc = ca;

    // Slab allocator. 64 nodes should be enough, as we'll have the Memory
    // Manager up and running before we really start mapping vaddresses.
    slab_init(&st->slabs, sizeof(struct paging_node), slab_default_refill);
    char* paging_buf = paging_heap_malloc(64 * sizeof(struct paging_node));
    if (paging_buf == NULL) {
        return LIB_ERR_VSPACE_INIT;
    }
    slab_grow(&st->slabs, paging_buf, 64 * sizeof(struct paging_node));

    // We don't have any L2 pagetables yet, thus make sure the flags are unset.
    for (int i = 0; i < L1_PAGETABLE_ENTRIES; ++i) {
        st->l2_pagetables[i].initialized = false;
    }

    size_t capacity = (size_t) (0xFFFFFFFF - start_vaddr);
    // Four initial empty nodes.
    st->head = (struct paging_node*) slab_alloc(&st->slabs);
    st->head->base = start_vaddr;
    st->head->size = capacity;
    st->head->type = NodeType_Free;
    st->head->prev = NULL;

    // Default L1 pagetable.
    st->l1_pagetable = pdir;

    // TODO (M4): Implement page fault handler that installs frames when a page fault
    // occurs and keeps track of the virtual address space.
    return SYS_ERR_OK;
}

/**
 * \brief This function initializes the paging for this domain
 * It is called once before main.
 */
errval_t paging_init(void)
{
    debug_printf("paging_init\n");

    // M2:
    // TODO: Where should the pdir capref come from?
    struct capref l1_cap = {
        .cnode = cnode_page,
        .slot = 0
    };
    paging_init_state(&current, VADDR_OFFSET, l1_cap,
            get_default_slot_allocator());
    set_current_paging_state(&current);

    // TODO (M4): initialize self-paging handler
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.

    return SYS_ERR_OK;
}

/**
 * \brief Initialize per-thread paging state
 */
static void handle_pagefault(int subtype,
        void* addr,
        arch_registers_state_t* regs,
        arch_registers_fpu_state_t* fpuregs)
{
    // TODO(razvan): What should be done based on subtype, regs, fpuregs?

    lvaddr_t vaddr = (lvaddr_t) addr;
    
    if (vaddr < BASE_PAGE_SIZE) {
        debug_printf("Thread attempted to dereference NULL-reserved memory at"
                "%u, killing it\n", vaddr);
        thread_exit(THREAD_EXIT_PAGEFAULT);
    }

    if (vaddr >= KERNEL_WINDOW) {
        // This is not the heap, kill the thread.
        debug_printf("Thread attempted to access non-heap address %u,"
                "killing it\n", vaddr);
        thread_exit(THREAD_EXIT_PAGEFAULT);
    }

    vaddr -= vaddr % BASE_PAGE_SIZE;

    struct paging_state* st = get_current_paging_state();

    if (!is_vregion_claimed(st, vaddr)) {
        // Some other thread has already allocated this?
        debug_printf("Pagefault handler: page at %u has not been claimed yet\n",
                vaddr);
        thread_exit(THREAD_EXIT_PAGEFAULT);
    }

    errval_t err;
    if (paging_should_refill_slabs(st)) {
        // Should first refill paging slabs before attempting to map.
        err = paging_refill_slabs(st);
        if (err_is_fail(err)) {
            debug_printf("Pagefault handler erred during paging_refill_slabs:"
                    "%s\n", err_getstring(err));
            thread_exit(THREAD_EXIT_PAGEFAULT);
        }
    }

    struct capref frame;
    size_t retsize;
    err = frame_alloc(&frame, BASE_PAGE_SIZE, &retsize);
    if (err_is_fail(err)) {
        debug_printf("Pagefault handler erred during frame_alloc: %s\n",
                err_getstring(err));
        thread_exit(THREAD_EXIT_PAGEFAULT);
    }

    err = paging_map_fixed(get_current_paging_state(), vaddr, frame, retsize);
    if (err_is_fail(err)) {
        debug_printf("Pagefault handler erred during paging_map_fixed: %s\n",
                err_getstring(err));
        // TODO(razvan): free frame before killing thread.
        thread_exit(THREAD_EXIT_PAGEFAULT);
    }
}

static void default_exception_handler(enum exception_type type, int subtype,
        void *addr, arch_registers_state_t *regs,
        arch_registers_fpu_state_t *fpuregs)
{
    switch (type) {
        case EXCEPT_PAGEFAULT:
            handle_pagefault(subtype, addr, regs, fpuregs);
            break;
        default:
            debug_printf("Unhandled exception type %d. Killing thread.\n",
                    type);
            thread_exit(THREAD_EXIT_UNHANDLED_EXCEPTION);
    }
}

void paging_init_onthread(struct thread *t)
{
    // TODO (M4): setup exception handler for thread `t'.
    static char stack_base[8192 * 4] = {0};
    char* stack_top = stack_base + 8192 * 2;
    t->exception_stack = (void *) stack_base;
    t->exception_stack_top = (void *) stack_top;
    t->exception_handler = (exception_handler_fn) default_exception_handler;
    // CHECK("init.c#barrelfish_init_onthread: thread_set_exception_handler (2)",
    //         thread_set_exception_handler(
    //                 (exception_handler_fn) default_exception_handler,
    //                 NULL,
    //                 (void*) stack_base, (void*) stack_top,
    //                 NULL, NULL));
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, size_t size)
{
    void *base;
    // TODO: This looks like a bug: calling paging_alloc directly should not
    // return a virtual memory area that has also been MAPPED TO. Or, conversely,
    // paging_map_frame_attr should not call both paging_alloc & paging_map_fixed_attr.
    // TODO: This is now worked around by mapping in paging_region_map.
    size = ROUND_UP(size, BASE_PAGE_SIZE);
    if (size == 0) {
        size = BASE_PAGE_SIZE;
    }
    errval_t err = paging_alloc(st, &base, size);

    if (err_is_fail(err)) {
        debug_printf("paging_region_init: paging_alloc failed\n");
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }
    pr->base_addr    = (lvaddr_t)base;
    pr->current_addr = pr->base_addr;
    pr->region_size  = size;
    pr->st = st;
    pr->mapped = false;
    return SYS_ERR_OK;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size)
{
    lvaddr_t end_addr = pr->base_addr + pr->region_size;
    ssize_t rem = end_addr - pr->current_addr;
    if (rem > req_size) {
        // ok
        *retbuf = (void*)pr->current_addr;
        *ret_size = req_size;
        pr->current_addr += req_size;
    } else if (rem > 0) {
        *retbuf = (void*)pr->current_addr;
        *ret_size = rem;
        pr->current_addr += rem;
        debug_printf("exhausted paging region, "
                "expect badness on next allocation\n");
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }
    return SYS_ERR_OK;
}

/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * NOTE: Implementing this function is optional.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes)
{
    // TIP: you will need to keep track of possible holes in the region
    // TODO(razvan): implement while paging_unmap is working.
    return SYS_ERR_OK;
}

bool paging_should_refill_slabs(struct paging_state *st)
{
    return slab_freecount(&st->slabs) < 6;
}

/**
 *
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    // TODO: M2 Implement this function
    struct paging_node *node = st->head;
    while (node != NULL) {
        if (node->type == NodeType_Free && node->size >= bytes) {
            // Claim the node.
            *buf = (void*) node->base;
            node->type = NodeType_Claimed;

            if (node->size > bytes) {
                // Split it.
                struct paging_node *new_node = (struct paging_node*) slab_alloc(&st->slabs);
                new_node->type = NodeType_Free;
                new_node->base = node->base + bytes;
                new_node->size = node->size - bytes;
                new_node->next = node->next;
                new_node->prev = node;
                if (node->next != NULL) {
                    node->next->prev = new_node;
                }
                node->next = new_node;
                node->size = bytes;
            }

            return SYS_ERR_OK;
        }
        node = node->next;
    }
    *buf = NULL;
    return LIB_ERR_VREGION_NOT_FOUND;
}

errval_t paging_refill_slabs(struct paging_state* st)
{
    return slab_refill_no_pagefault(&st->slabs, NULL_CAP, BASE_PAGE_SIZE);
}

/**
 * \brief map a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 */
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                               size_t bytes, struct capref frame,
                               int flags, void *arg1, void *arg2)
{
    if (paging_should_refill_slabs(st)) {
        CHECK("paging_map_frame_attr: paging_refill_slabs",
                paging_refill_slabs(st));
    }
    errval_t err = paging_alloc(st, buf, bytes);
    if (err_is_fail(err)) {
        return err;
    }
    return paging_map_fixed_attr(st, (lvaddr_t) (*buf), frame, bytes, flags);
}

errval_t
slab_refill_no_pagefault(struct slab_allocator *slabs, struct capref frame, size_t minbytes)
{
    minbytes = ROUND_UP(minbytes, BASE_PAGE_SIZE);
    if (minbytes == 0) {
        minbytes = BASE_PAGE_SIZE;
    }
    char* buf = paging_heap_malloc(minbytes);
    if (buf == NULL) {
        return LIB_ERR_SLAB_REFILL;
    }
    slab_grow(slabs, buf, minbytes);
    return SYS_ERR_OK;
}

// Whether the vregion given by vaddr and size is of type NodeType_Claimed.
bool is_vregion_claimed(struct paging_state* st, lvaddr_t vaddr)
{
    struct paging_node* node = st->head;
    while (node != NULL) {
        if (node->base <= vaddr && node->base + node->size > vaddr) {
            return node->type == NodeType_Claimed;
        }
        node = node->next;
    }
    return false;
}

/**
 * \brief map a user provided frame at user provided VA.
 * TODO(M1): Map a frame assuming all mappings will fit into one L2 pt
 * TODO(M2): General case
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
    /* Step 1: Check if the virtual memory area wanted by the user is in fact
               free (check corresponding page_node). */
    struct paging_node *node = st->head;
    while (bytes > 0) {
        if (node == NULL) {
            // Couldn't find node, err out.
            return LIB_ERR_VREGION_MAP_FIXED;
        }
        if (node->type == NodeType_Allocated) {
            // Skip node if allocated.
            node = node->next;
            continue;
        }
        if (node->base > vaddr || node->base + node->size < vaddr + bytes) {
            // Current node can't hold the desired vregion.
            node = node->next;
            continue;
        }

        /* Step 2: Mark node as allocated & split it. */
        // TODO: If further steps fail and this function returns without success
        //       we should free the node & merge it back.
        enum nodetype old_type = node->type;
        node->type = NodeType_Allocated;
        if (node->base + node->size > vaddr + bytes) {
            // Need new (free) node to the right;
            struct paging_node *right = (struct paging_node*) slab_alloc(&st->slabs);
            right->type = old_type;
            right->base = vaddr + bytes;
            right->size = node->size - (vaddr - node->base) - bytes;
            right->next = node->next;
            right->prev = node;
            if (node->next != NULL) {
                node->next->prev = right;
            }
            node->next = right;
            node->size -= right->size;
        }

        if (vaddr > node->base) {
            // Need new (free) node to the left.
            struct paging_node *left = (struct paging_node*) slab_alloc(&st->slabs);
            left->type = old_type;
            left->base = node->base;
            left->size = vaddr - node->base;
            left->next = node;
            left->prev = node->prev;
            if (node->prev != NULL) {
                node->prev->next = left;
            }
            if (st->head == node) {
                st->head = left;
            }
            node->prev = left;
            node->base = vaddr;
            node->size -= left->size;
        }

        /* Step 2: Compute & (if needed) create all the necessary L2 tables and
           sub-frames. */
        uint32_t mapped_size = 0;
        errval_t err;
        while (bytes > 0) {
            struct capref l2_cap;
            // Get index of next L2 pagetable to map into.
            uint16_t l2_index = ARM_L1_OFFSET(vaddr);

            if (st->l2_pagetables[l2_index].initialized) {
                l2_cap = st->l2_pagetables[l2_index].cap;
            } else {
                // Need to allocate a new L2 pagetable.
                err = arml2_alloc(st, &l2_cap);
                if (err_is_fail(err)) {
                    return err;
                }

                // Map newly created L2 to L1.
                struct capref l2_to_l1;
                err = st->slot_alloc->alloc(st->slot_alloc, &l2_to_l1);
                if (err_is_fail(err)) {
                    DEBUG_ERR(err, "slot_alloc for mapping L2 to L1\n");
                    return err;
                }
                err = vnode_map(st->l1_pagetable, l2_cap, l2_index,
                        VREGION_FLAGS_READ_WRITE, 0, 1, l2_to_l1);
                if (err_is_fail(err)) {
                    DEBUG_ERR(err, "Mapping L2 to L1");
                    return err;
                }

                if (st->mapping_cb) {
                    err = st->mapping_cb(st->mapping_state, l2_to_l1);
                    if (err_is_fail(err)) {
                        DEBUG_ERR(err, "Copying mapping l2_to_l1 to child");
                        return err;
                    }
                }

                st->l2_pagetables[l2_index].cap = l2_cap;
                st->l2_pagetables[l2_index].initialized = true;
            }

            // Get index frame should start at in current L2 table.
            uint16_t frame_index = ARM_L2_OFFSET(vaddr);
            uint16_t l2_entries_left = ARM_L2_MAX_ENTRIES - frame_index;
            size_t size_to_map = (bytes < l2_entries_left * BASE_PAGE_SIZE)
                    ? bytes
                    : l2_entries_left * BASE_PAGE_SIZE;

            /* Step 3: Perform mapping. */
            struct capref frame_to_l2;
            err = st->slot_alloc->alloc(st->slot_alloc, &frame_to_l2);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "slot_alloc for mapping frame to L2\n");
                return err;
            }
            err = vnode_map(l2_cap,
                    frame/*cap_to_map*/,
                    frame_index,
                    flags,
                    mapped_size,
                    size_to_map / BASE_PAGE_SIZE,
                    frame_to_l2);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "Mapping frame to L2");  
                return err;
            }
            if (st->mapping_cb) {
                err = st->mapping_cb(st->mapping_state, frame_to_l2);
                if (err_is_fail(err)) {
                    DEBUG_ERR(err, "Copying mapping frame_to_l2 to child");
                    return err;
                }
            }

            mapped_size += size_to_map;
            bytes -= size_to_map;
            vaddr += size_to_map;
        }
        if (bytes > 0) {
            node = node->next;
        }
    }

    return SYS_ERR_OK;
}

/**
 * \brief unmap region starting at address `region`.
 * NOTE: Implementing this function is optional.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    return SYS_ERR_OK;
}
