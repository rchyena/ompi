/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2014 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "btl_vader.h"

#include "opal/include/opal/align.h"
#include "opal/mca/memchecker/base/base.h"

#if OPAL_BTL_VADER_HAVE_XPMEM

int mca_btl_vader_xpmem_init (void)
{
    mca_btl_vader_component.my_seg_id = xpmem_make (0, VADER_MAX_ADDRESS, XPMEM_PERMIT_MODE, (void *)0666);
    if (-1 == mca_btl_vader_component.my_seg_id) {
        return OPAL_ERR_NOT_AVAILABLE;
    }

    mca_btl_vader.super.btl_get = mca_btl_vader_get_xpmem;
    mca_btl_vader.super.btl_put = mca_btl_vader_put_xpmem;

    return OPAL_SUCCESS;
}

/* look up the remote pointer in the peer rcache and attach if
 * necessary */
mca_rcache_base_registration_t *vader_get_registation (struct mca_btl_base_endpoint_t *ep, void *rem_ptr,
                                                       size_t size, int flags, void **local_ptr)
{
    mca_rcache_base_vma_module_t *vma_module = ep->segment_data.xpmem.vma_module;
    mca_rcache_base_registration_t *regs[10], *reg = NULL;
    xpmem_addr_t xpmem_addr;
    uintptr_t base, bound;
    uint64_t attach_align = 1 << mca_btl_vader_component.log_attach_align;
    int rc, i;

    /* protect rcache access */
    OPAL_THREAD_LOCK(&ep->lock);

    /* use btl/self for self communication */
    assert (ep->peer_smp_rank != MCA_BTL_VADER_LOCAL_RANK);

    base = OPAL_DOWN_ALIGN((uintptr_t) rem_ptr, attach_align, uintptr_t);
    bound = OPAL_ALIGN((uintptr_t) rem_ptr + size - 1, attach_align, uintptr_t) + 1;
    if (OPAL_UNLIKELY(bound > VADER_MAX_ADDRESS)) {
        bound = VADER_MAX_ADDRESS;
    }

    /* several segments may match the base pointer */
    rc = mca_rcache_base_vma_find_all (vma_module, (void *) base, bound - base, regs, 10);
    for (i = 0 ; i < rc ; ++i) {
        if (bound <= (uintptr_t)regs[i]->bound && base  >= (uintptr_t)regs[i]->base) {
            (void)opal_atomic_add (&regs[i]->ref_count, 1);
            reg = regs[i];
            goto reg_found;
        }

        if (regs[i]->flags & MCA_RCACHE_FLAGS_PERSIST) {
            continue;
        }

        /* remove this pointer from the rcache and decrement its reference count
           (so it is detached later) */
        rc = mca_rcache_base_vma_delete (vma_module, regs[i]);
        if (OPAL_UNLIKELY(0 != rc)) {
            /* someone beat us to it? */
            break;
        }

        /* start the new segment from the lower of the two bases */
        base = (uintptr_t) regs[i]->base < base ? (uintptr_t) regs[i]->base : base;

        (void)opal_atomic_add (&regs[i]->ref_count, -1);

        if (OPAL_LIKELY(0 == regs[i]->ref_count)) {
            /* this pointer is not in use */
            (void) xpmem_detach (regs[i]->rcache_context);
            OBJ_RELEASE(regs[i]);
        }

        break;
    }

    reg = OBJ_NEW(mca_rcache_base_registration_t);
    if (OPAL_LIKELY(NULL != reg)) {
        /* stick around for awhile */
        reg->ref_count = 2;
        reg->base  = (unsigned char *) base;
        reg->bound = (unsigned char *) bound;
        reg->flags = flags;

#if defined(HAVE_SN_XPMEM_H)
        xpmem_addr.id     = ep->segment_data.xpmem.apid;
#else
        xpmem_addr.apid   = ep->segment_data.xpmem.apid;
#endif
        xpmem_addr.offset = base;

        reg->rcache_context = xpmem_attach (xpmem_addr, bound - base, NULL);
        if (OPAL_UNLIKELY((void *)-1 == reg->rcache_context)) {
            OPAL_THREAD_UNLOCK(&ep->lock);
            OBJ_RELEASE(reg);
            return NULL;
        }

        opal_memchecker_base_mem_defined (reg->rcache_context, bound - base);

        mca_rcache_base_vma_insert (vma_module, reg, 0);
    }

reg_found:
    opal_atomic_wmb ();
    *local_ptr = (void *) ((uintptr_t) reg->rcache_context +
                           (ptrdiff_t)((uintptr_t) rem_ptr - (uintptr_t) reg->base));

    OPAL_THREAD_UNLOCK(&ep->lock);

    return reg;
}

void vader_return_registration (mca_rcache_base_registration_t *reg, struct mca_btl_base_endpoint_t *ep)
{
    mca_rcache_base_vma_module_t *vma_module = ep->segment_data.xpmem.vma_module;
    int32_t ref_count;

    ref_count = opal_atomic_add_32 (&reg->ref_count, -1);
    if (OPAL_UNLIKELY(0 == ref_count && !(reg->flags & MCA_RCACHE_FLAGS_PERSIST))) {
        /* protect rcache access */
        OPAL_THREAD_LOCK(&ep->lock);
        mca_rcache_base_vma_delete (vma_module, reg);
        OPAL_THREAD_UNLOCK(&ep->lock);

        opal_memchecker_base_mem_noaccess (reg->rcache_context, (uintptr_t)(reg->bound - reg->base));
        (void)xpmem_detach (reg->rcache_context);
        OBJ_RELEASE (reg);
    }
}

#endif /* OPAL_BTL_VADER_HAVE_XPMEM */
