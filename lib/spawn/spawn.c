#include <aos/aos.h>
#include <spawn/spawn.h>

#include <elf/elf.h>
#include <aos/dispatcher_arch.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <barrelfish_kpi/domain_params.h>
#include <spawn/multiboot.h>

#define DPRINT(fmt, args...)  debug_printf("spawn: " fmt "\n", args)

#define CHECK(where, err)  if (err_is_fail(err)) {  \
                               DPRINT("ERROR %s: %s", where, err_getstring(err));  \
                               return err;  \
                           }

extern struct bootinfo *bi;

void setup_cspace(struct spawninfo *si) {
    CHECK("cnode_create_l1", cnode_create_l1(&(si->l1_cap), &(si->l1_cnoderef)));

    // Create TASKCN
    CHECK("taskcn", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_TASKCN, &(si->taskcn)));

    // Create SLOT PAGECN
    CHECK("pagecn", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_PAGECN, &(si->pagecn)))

    // Create SLOT BASE PAGE CN
    CHECK("base_pagecn", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_BASE_PAGE_CN, &(si->base_pagecn)));

    // Create SLOT ALLOC 0
    CHECL("alloc0", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC0, &(si->alloc0)));

    // Create SLOT ALLOC 1
    CHECK("alloc1", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC1, &(si->alloc1)));

    // Create SLOT ALLOC 2
    CHECK("alloc2", cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC2, &(si->alloc2)));

    // Create SLOT DISPATCHER
    si->dispatcher.cnode = si->taskcn;
    si->dispatcher.slot = TASKCN_SLOT_DISPATCHER;

    // Create SLOT ROOTCN
    si->rootcn.cnode = si->taskcn;
    si->rootcn.slot = TASKCN_SLOT_ROOTCN;

    // Create SLOT DISPFRAME
    si->dispframe.cnode = si->taskcn;
    si->dispframe.slot = TASKCN_SLOT_DISPFRAME;

    // Create SLOT ARGSPG
    si->argspg.cnode = si->taskcn;
    si->argspg.slot = TASKCN_SLOT_ARGSPAGE;

    cap_retype(si->selfep, si->dispatcher, 0, ObjType_EndPoint, 0, 1);

}

void setup_vspace(struct spawninfo *si) {
    si->l1_pagetable.cnode = si->pagecn;
    si->l1_pagetable.slot = 0;

//     //CHECK("", cap_copy(si->l1_pagetable, cnode_page));
     vnode_create(, ObjType_VNode_ARM_l1);
}

// TODO(M2): Implement this function such that it starts a new process
// TODO(M4): Build and pass a messaging channel to your child process
errval_t spawn_load_by_name(void * binary_name, struct spawninfo * si) {

    DPRINT("loading and starting: %s", binary_name);

    // Init spawninfo
    memset(si, 0, sizeof(*si));
    si->binary_name = binary_name;

    // - Get the binary from multiboot image

    struct mem_region *module = multiboot_find_module(bi, binary_name);
    if (!module) {
        DPRINT("Module %s not found", binary_name);
        return SPAWN_ERR_FIND_MODULE;
    }

    struct capref child_frame = {
        .cnode = cnode_module,
        .slot = module->mrmod_slot,
    };

    // - Map multiboot module in your address space

    struct frame_identity child_frame_id;
    CHECK("identifying frame",
          frame_identify(child_frame, &child_frame_id));

    void *mapped_elf;
    CHECK("mapping frame",
          paging_map_frame(get_current_paging_state(), &mapped_elf,
                           child_frame_id.bytes, child_frame, NULL, NULL));
    DPRINT("ELF header: %0x %c %c %c", ((char*)mapped_elf)[0], ((char*)mapped_elf)[1], ((char*)mapped_elf)[2], ((char*)mapped_elf)[3]);

    struct Elf32_Ehdr *elf_header = mapped_elf;
    if (!IS_ELF(*elf_header)) {
        DPRINT("Module %s is not an ELF executable", binary_name);
        return ELF_ERR_HEADER;
    }

    // - Setup childs cspace

    setup_cspace(si);
    //CHECK("Setup CSPACE", setup_cspace(si));
    // - Setup childs vspace
    // - Load the ELF binary
    // - Setup dispatcher
    // - Setup environment
    // - Make dispatcher runnable

    return SYS_ERR_OK;
}
